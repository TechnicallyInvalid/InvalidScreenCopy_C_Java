// Exercise the real routing/clock bridge with fake transport and capture only.
// No SDL audio subsystem, capture device, TCC query, or permission request runs.
#include "recording_microphones.h"
#include "controller.h"
#include "device_msg.h"
#include "local_microphone.h"
#include "recording.h"
#include "receiver.h"
#include <assert.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <libavutil/time.h>

static struct sc_recording *recording;
static struct sc_controller *controller;
static const struct sc_recording_microphone_ops *ops;
static void *backend_data;
static const struct sc_receiver_microphone_callbacks *receiver_callbacks;
static void *receiver_data;
static struct sc_local_microphone_callbacks capture_callbacks;
static void *capture_data;
static bool output_enabled, send_ok = true, open_ok = true, poll_ok = true;
static bool capture_open;
static int capture_token;
static unsigned starts, stops, messages, interrupts, local_frames;
static unsigned android_frames, android_statuses, errors;
static unsigned ack_mode; // 0=none, 1=matching STOPPED, 2=stale, 3=matching ERROR
static uint32_t expected_session, last_session;
static int64_t now_us = 1000000, last_pts;
static char last_error[256];
static struct sc_control_msg last_message;

static bool fake_push(struct sc_controller *c, const struct sc_control_msg *msg) {
    assert(c == controller);
    last_message = *msg;
    ++messages;
    if (send_ok && msg->type == SC_CONTROL_MSG_TYPE_SET_ANDROID_MIC
            && !msg->set_android_mic.enabled && ack_mode) {
        uint32_t session = msg->set_android_mic.session - (ack_mode == 2);
        uint8_t status = ack_mode == 3 ? SC_MIC_STATUS_ERROR : SC_MIC_STATUS_STOPPED;
        receiver_callbacks->on_status(session, status, "Fake capture released", receiver_data);
    }
    return send_ok;
}
static bool fake_interrupt(sc_socket socket) {
    assert(socket == controller->receiver.control_socket);
    ++interrupts;
    return true;
}
static int64_t fake_now(void) { return now_us; }
static void fake_receiver(struct sc_receiver *r,
    const struct sc_receiver_microphone_callbacks *callbacks, void *userdata) {
    assert(r == &controller->receiver);
    receiver_callbacks = callbacks;
    receiver_data = userdata;
}
static void fake_backend(struct sc_recording *r,
    const struct sc_recording_microphone_ops *backend, void *userdata, bool output) {
    assert(r == recording);
    ops = backend;
    backend_data = userdata;
    output_enabled = output;
}
static struct sc_local_microphone *fake_start(bool selected,
    const struct sc_local_microphone_callbacks *callbacks, void *userdata,
    char *error, size_t size) {
    assert(selected && !capture_open);
    ++starts;
    if (!open_ok) {
        snprintf(error, size, "Fake Local Mic permission/open failure.");
        return NULL;
    }
    capture_callbacks = *callbacks;
    capture_data = userdata;
    capture_open = true;
    return (struct sc_local_microphone *) &capture_token;
}
static bool fake_poll(struct sc_local_microphone *m, char *error, size_t size) {
    assert(m == (struct sc_local_microphone *) &capture_token && capture_open);
    if (!poll_ok) snprintf(error, size, "Fake Local Mic disconnected.");
    return poll_ok;
}
static void fake_stop(struct sc_local_microphone *m) {
    assert(m == (struct sc_local_microphone *) &capture_token && capture_open);
    ++stops;
    capture_open = false;
}
static void fake_local_pcm(struct sc_recording *r, uint32_t session, int64_t pts,
    const float *pcm, size_t frames) {
    assert(r == recording && pcm && frames == 4);
    last_session = session;
    last_pts = pts;
    if (session == expected_session) ++local_frames;
}
static void fake_error(struct sc_recording *r, uint32_t session, const char *message) {
    assert(r == recording && message);
    last_session = session;
    snprintf(last_error, sizeof(last_error), "%s", message);
    if (session == expected_session) ++errors;
}
static void fake_android_pcm(struct sc_recording *r, uint32_t session, int64_t pts,
    const uint8_t *pcm, uint16_t bytes) {
    assert(r == recording && pcm && bytes == 8);
    last_session = session;
    last_pts = pts;
    if (session == expected_session) ++android_frames;
}
static void fake_android_status(struct sc_recording *r, uint32_t session,
    uint8_t status, const char *message) {
    assert(r == recording && status <= SC_MIC_STATUS_ERROR && message);
    last_session = session;
    if (session == expected_session) ++android_statuses;
}

#define sc_controller_push_msg fake_push
#define net_interrupt fake_interrupt
#define av_gettime_relative fake_now
#define sc_receiver_set_microphone_callbacks fake_receiver
#define sc_recording_set_microphone_backend fake_backend
#define sc_local_microphone_start fake_start
#define sc_local_microphone_poll fake_poll
#define sc_local_microphone_stop fake_stop
#define sc_recording_local_pcm fake_local_pcm
#define sc_recording_microphone_error fake_error
#define sc_recording_android_pcm fake_android_pcm
#define sc_recording_android_status fake_android_status
#include "../src/recording_microphones.c"
#undef sc_controller_push_msg
#undef net_interrupt
#undef av_gettime_relative
#undef sc_receiver_set_microphone_callbacks
#undef sc_recording_set_microphone_backend
#undef sc_local_microphone_start
#undef sc_local_microphone_poll
#undef sc_local_microphone_stop
#undef sc_recording_local_pcm
#undef sc_recording_microphone_error
#undef sc_recording_android_pcm
#undef sc_recording_android_status

int main(void) {
    assert(SDL_Init(SDL_INIT_EVENTS));
    struct sc_recording r = {0};
    struct sc_controller c = {0};
    recording = &r;
    controller = &c;
#ifdef SC_SOCKET_CLOSE_ON_INTERRUPT
    struct sc_socket_wrapper fake_socket = {.socket = 42, .closed = ATOMIC_FLAG_INIT};
    c.receiver.control_socket = &fake_socket;
#else
    c.receiver.control_socket = 42;
#endif
    expected_session = 11;
    struct sc_recording_microphones *bridge =
        sc_recording_microphones_create(&r, &c, true);
    assert(bridge && ops && receiver_callbacks && output_enabled);
    assert(starts == 0 && messages == 0); // configuring must never open capture
    char error[256];
    assert(!ops->start_local(backend_data, expected_session, error, sizeof(error)));
    assert(starts == 0 && strstr(error, "synchronization"));

    // Android-only controls and clock calibration cannot open a local device.
    assert(ops->set_android(backend_data, true, expected_session));
    assert(last_message.type == SC_CONTROL_MSG_TYPE_SET_ANDROID_MIC);
    assert(last_message.set_android_mic.enabled);
    assert(last_message.set_android_mic.session == expected_session);
    assert(ops->query_clock(backend_data, expected_session, 1));
    assert(last_message.type == SC_CONTROL_MSG_TYPE_CLOCK_QUERY);
    now_us += 2000;
    receiver_callbacks->on_clock(expected_session - 1, 1, 5001000, receiver_data);
    assert(!ops->clock_ready(backend_data, expected_session, error, sizeof(error)));
    receiver_callbacks->on_clock(expected_session, 1, 5001000, receiver_data);
    assert(ops->clock_ready(backend_data, expected_session, error, sizeof(error)));
    assert(starts == 0);
    assert(!ops->start_local(backend_data, expected_session + 1, error, sizeof(error)));
    assert(starts == 0);

    // The first real adapter start occurs only through explicit recording ops.
    assert(ops->start_local(backend_data, expected_session, error, sizeof(error)));
    assert(starts == 1 && capture_open);
    assert(!ops->start_local(backend_data, expected_session, error, sizeof(error)));
    assert(starts == 1); // never open twice for one recording
    float samples[4] = {.1f, -.1f, .2f, -.2f};
    now_us = 1005000;
    assert(capture_callbacks.on_pcm(capture_data, samples, 4, 1003000));
    assert(local_frames == 1 && last_session == expected_session && last_pts == 5003000);
    assert(ops->poll_local(backend_data, error, sizeof(error)));
    capture_callbacks.on_error(capture_data, "Fake capture error");
    assert(errors == 1 && strstr(last_error, "Fake capture error"));
    poll_ok = false;
    assert(!ops->poll_local(backend_data, error, sizeof(error)));
    assert(strstr(error, "disconnected"));
    poll_ok = true;
    ops->stop_local(backend_data);
    ops->stop_local(backend_data);
    assert(stops == 1 && !capture_open);

    // Session identities are preserved, so the recording owner can reject old
    // status/PCM instead of attributing them to the current selected source.
    uint8_t pcm[8] = {0};
    receiver_callbacks->on_pcm(expected_session - 1, 5000000, pcm, 8, receiver_data);
    assert(last_session == expected_session - 1 && android_frames == 0);
    receiver_callbacks->on_pcm(expected_session, 5000000, pcm, 8, receiver_data);
    assert(android_frames == 1);
    receiver_callbacks->on_status(expected_session - 1, 1, "ready", receiver_data);
    assert(android_statuses == 0);
    receiver_callbacks->on_status(expected_session, 1, "ready", receiver_data);
    assert(android_statuses == 1);

    // A successful enqueue alone does not confirm microphone release. Only a
    // terminal acknowledgment for this session may finish the stop operation.
    ack_mode = 1;
    assert(ops->set_android(backend_data, false, expected_session));
    assert(interrupts == 0);
    assert(ops->set_android(backend_data, true, expected_session));
    ack_mode = 2;
    sc_tick waiting_since = sc_tick_now();
    assert(!ops->set_android(backend_data, false, expected_session));
    assert(sc_tick_now() - waiting_since >= SC_TICK_FROM_MS(1000));
    assert(interrupts == 1);
    assert(ops->set_android(backend_data, true, expected_session));
    ack_mode = 3;
    assert(ops->set_android(backend_data, false, expected_session));
    assert(interrupts == 1);
    ack_mode = 0;

    // An Android stop that cannot be enqueued interrupts control, ensuring the
    // remote EOF/lease cleanup path, rather than silently leaving capture live.
    unsigned previous_interrupts = interrupts;
    send_ok = false;
    assert(!ops->set_android(backend_data, true, expected_session));
    assert(interrupts == previous_interrupts);
    assert(!ops->set_android(backend_data, false, expected_session));
    assert(interrupts == previous_interrupts + 1);
    send_ok = true;

    // Expired or previous-session clock data cannot reopen a microphone.
    now_us += 11000000;
    assert(!ops->start_local(backend_data, expected_session, error, sizeof(error)));
    assert(starts == 1);
    ++expected_session;
    assert(ops->query_clock(backend_data, expected_session, 1));
    int64_t sent = now_us;
    now_us += 2000;
    receiver_callbacks->on_clock(expected_session - 1, 1, sent + 4001000, receiver_data);
    assert(!ops->clock_ready(backend_data, expected_session, error, sizeof(error)));
    receiver_callbacks->on_clock(expected_session, 1, sent + 4001000, receiver_data);
    open_ok = false;
    assert(!ops->start_local(backend_data, expected_session, error, sizeof(error)));
    assert(starts == 2 && !capture_open && strstr(error, "permission/open"));
    open_ok = true;
    assert(ops->start_local(backend_data, expected_session, error, sizeof(error)));
    assert(starts == 3 && capture_open);
    now_us += 11000000;
    assert(!capture_callbacks.on_pcm(capture_data, samples, 4, now_us));
    assert(local_frames == 1 && strstr(last_error, "synchronization was lost"));
    assert(ops->query_clock(backend_data, expected_session, 2));
    sent = now_us;
    now_us += 2000;
    receiver_callbacks->on_clock(expected_session, 2, sent + 4001000, receiver_data);
    assert(ops->clock_ready(backend_data, expected_session, error, sizeof(error)));
    unsigned previous_errors = errors;
    unsigned previous_messages = messages;
    receiver_callbacks->on_closed(true, receiver_data);
    assert(errors == previous_errors + 1 && last_session == expected_session);
    assert(strstr(last_error, "connection closed"));
    assert(!ops->clock_ready(backend_data, expected_session, error, sizeof(error)));
    assert(!ops->query_clock(backend_data, expected_session, 3));
    assert(!ops->set_android(backend_data, true, expected_session));
    assert(messages == previous_messages);
    assert(!capture_callbacks.on_pcm(capture_data, samples, 4, now_us));
    assert(local_frames == 1);
    sc_recording_microphones_destroy(bridge);
    assert(stops == 2 && !capture_open);

    bridge = sc_recording_microphones_create(&r, &c, false);
    assert(bridge && !output_enabled && starts == 3);
    sc_recording_microphones_destroy(bridge);
    assert(stops == 2);
    SDL_Quit();
    return 0;
}
