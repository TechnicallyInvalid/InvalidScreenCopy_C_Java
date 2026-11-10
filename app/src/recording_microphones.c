#include "recording_microphones.h"

#include <stdlib.h>
#include <stdio.h>
#include <libavutil/time.h>

#include "controller.h"
#include "device_msg.h"
#include "local_microphone.h"
#include "recording.h"
#include "recording_clock.h"
#include "receiver.h"

struct sc_recording_microphones {
    struct sc_recording *recording;
    struct sc_controller *controller;
    struct sc_local_microphone *local; // lifecycle worker ownership
    uint32_t local_session; // stable until stop joins capture callbacks
    sc_mutex mutex; // clock and active session shared with receiver/capture
    sc_cond cond;
    uint32_t session;
    uint32_t released_session;
    bool control_closed;
    struct sc_recording_clock clock;
};

static bool
set_android(void *userdata, bool enabled, uint32_t session) {
    struct sc_recording_microphones *b = userdata;
    if (enabled) {
        sc_mutex_lock(&b->mutex);
        if (b->control_closed) {
            sc_mutex_unlock(&b->mutex);
            return false;
        }
        b->session = session;
        b->released_session = 0;
        sc_mutex_unlock(&b->mutex);
    }
    struct sc_control_msg msg = {.type = SC_CONTROL_MSG_TYPE_SET_ANDROID_MIC};
    msg.set_android_mic.enabled = enabled;
    msg.set_android_mic.session = session;
    bool sent = sc_controller_push_msg(b->controller, &msg);
    if (!sent && !enabled) {
        // A missing stop command must not leave the phone microphone running.
        // EOF makes the server release capture; its independent heartbeat lease
        // also expires if a broken transport cannot deliver that EOF promptly.
        net_interrupt(b->controller->receiver.control_socket);
    }
    if (sent && !enabled) {
        // Enqueuing Stop is not proof that the phone has closed capture. Keep
        // the clip busy until the server confirms release. A stalled transport
        // closes the display connection instead of claiming the mic is off.
        sc_tick deadline = sc_tick_now() + SC_TICK_FROM_MS(1500);
        sc_mutex_lock(&b->mutex);
        while (b->released_session != session && !b->control_closed) {
            if (!sc_cond_timedwait(&b->cond, &b->mutex, deadline)) break;
        }
        bool released = b->released_session == session;
        sc_mutex_unlock(&b->mutex);
        if (!released) {
            net_interrupt(b->controller->receiver.control_socket);
            return false;
        }
    }
    return sent;
}

static bool
query_clock(void *userdata, uint32_t session, uint32_t sequence) {
    struct sc_recording_microphones *b = userdata;
    sc_mutex_lock(&b->mutex);
    if (b->control_closed) {
        sc_mutex_unlock(&b->mutex);
        return false;
    }
    if (sequence == 1 || b->session != session) {
        sc_recording_clock_reset(&b->clock);
        b->session = session;
    }
    bool ok = sc_recording_clock_begin(&b->clock, sequence, av_gettime_relative());
    sc_mutex_unlock(&b->mutex);
    if (!ok) return false;
    struct sc_control_msg msg = {.type = SC_CONTROL_MSG_TYPE_CLOCK_QUERY};
    msg.clock_query.session = session;
    msg.clock_query.sequence = sequence;
    return sc_controller_push_msg(b->controller, &msg);
}

static bool
clock_ready(void *userdata, uint32_t session, char *error, size_t size) {
    struct sc_recording_microphones *b = userdata;
    if (size) error[0] = '\0';
    sc_mutex_lock(&b->mutex);
    bool ready = !b->control_closed && b->session == session
        && sc_recording_clock_ready(&b->clock, av_gettime_relative());
    if (b->control_closed) snprintf(error, size, "Phone control connection is closed.");
    sc_mutex_unlock(&b->mutex);
    return ready;
}

static bool
local_pcm(void *userdata, const float *samples, size_t frames, int64_t host_pts) {
    struct sc_recording_microphones *b = userdata;
    int64_t device_pts;
    sc_mutex_lock(&b->mutex);
    bool ok = !b->control_closed && b->session == b->local_session
        && sc_recording_clock_map(&b->clock, host_pts, av_gettime_relative(), &device_pts);
    sc_mutex_unlock(&b->mutex);
    if (!ok) {
        sc_recording_microphone_error(b->recording, b->local_session,
            "Local Mic clock synchronization was lost. Recording stopped.");
        return false;
    }
    sc_recording_local_pcm(b->recording, b->local_session, device_pts, samples, frames);
    return true;
}

static void
local_error(void *userdata, const char *message) {
    struct sc_recording_microphones *b = userdata;
    sc_recording_microphone_error(b->recording, b->local_session, message);
}

static bool
start_local(void *userdata, uint32_t session, char *error, size_t size) {
    struct sc_recording_microphones *b = userdata;
    if (b->local || !clock_ready(b, session, error, size)) {
        snprintf(error, size, "Local Mic clock synchronization is not ready.");
        return false;
    }
    static const struct sc_local_microphone_callbacks callbacks = {
        .on_pcm = local_pcm, .on_error = local_error,
    };
    b->local_session = session;
    b->local = sc_local_microphone_start(true, &callbacks, b, error, size);
    return b->local != NULL;
}

static bool
poll_local(void *userdata, char *error, size_t size) {
    struct sc_recording_microphones *b = userdata;
    if (!clock_ready(b, b->local_session, error, size)) {
        snprintf(error, size, "Local Mic clock synchronization expired. Recording stopped.");
        return false;
    }
    return sc_local_microphone_poll(b->local, error, size);
}

static void
stop_local(void *userdata) {
    struct sc_recording_microphones *b = userdata;
    if (b->local) {
        sc_local_microphone_stop(b->local);
        b->local = NULL;
    }
}

static void
android_status(uint32_t session, uint8_t status, const char *message, void *userdata) {
    struct sc_recording_microphones *b = userdata;
    // Publish any capture failure before waking the lifecycle worker waiting
    // for release, so an ERROR received during Stop cannot become "Saved".
    sc_recording_android_status(b->recording, session, status, message);
    // Java emits both terminal statuses only after releasing AudioRecord.
    if (status == SC_MIC_STATUS_STOPPED || status == SC_MIC_STATUS_ERROR) {
        sc_mutex_lock(&b->mutex);
        if (session == b->session) {
            b->released_session = session;
            sc_cond_broadcast(&b->cond);
        }
        sc_mutex_unlock(&b->mutex);
    }
}

static void
android_pcm(uint32_t session, int64_t pts, const uint8_t *pcm, uint16_t size, void *userdata) {
    struct sc_recording_microphones *b = userdata;
    sc_recording_android_pcm(b->recording, session, pts, pcm, size);
}

static void
clock_reply(uint32_t session, uint32_t sequence, int64_t pts, void *userdata) {
    struct sc_recording_microphones *b = userdata;
    sc_mutex_lock(&b->mutex);
    if (!b->control_closed && session == b->session) {
        sc_recording_clock_complete(&b->clock, sequence, pts, av_gettime_relative());
    }
    sc_mutex_unlock(&b->mutex);
}

static void
control_closed(bool error, void *userdata) {
    (void) error;
    struct sc_recording_microphones *b = userdata;
    sc_mutex_lock(&b->mutex);
    uint32_t session = b->session;
    b->control_closed = true;
    sc_cond_broadcast(&b->cond);
    sc_mutex_unlock(&b->mutex);
    sc_recording_microphone_error(b->recording, session,
        "Phone control connection closed. Microphone recording stopped.");
}

struct sc_recording_microphones *
sc_recording_microphones_create(struct sc_recording *recording,
                                struct sc_controller *controller,
                                bool device_output) {
    struct sc_recording_microphones *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    if (!sc_mutex_init(&b->mutex)) {
        free(b);
        return NULL;
    }
    if (!sc_cond_init(&b->cond)) {
        sc_mutex_destroy(&b->mutex);
        free(b);
        return NULL;
    }
    b->recording = recording;
    b->controller = controller;
    sc_recording_clock_reset(&b->clock);
    static const struct sc_recording_microphone_ops ops = {
        .set_android = set_android, .query_clock = query_clock,
        .clock_ready = clock_ready, .start_local = start_local,
        .poll_local = poll_local, .stop_local = stop_local,
    };
    static const struct sc_receiver_microphone_callbacks callbacks = {
        .on_status = android_status, .on_pcm = android_pcm,
        .on_clock = clock_reply, .on_closed = control_closed,
    };
    sc_receiver_set_microphone_callbacks(&controller->receiver, &callbacks, b);
    sc_recording_set_microphone_backend(recording, &ops, b, device_output);
    return b;
}

void
sc_recording_microphones_destroy(struct sc_recording_microphones *b) {
    if (!b) return;
    stop_local(b);
    sc_cond_destroy(&b->cond);
    sc_mutex_destroy(&b->mutex);
    free(b);
}
