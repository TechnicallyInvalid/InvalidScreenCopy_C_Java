#import <AVFoundation/AVFoundation.h>
#include "local_microphone.h"
#include <SDL3/SDL.h>
#include <libavutil/time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_LOCAL_MIC_RATE 48000
#define SC_LOCAL_MIC_CHUNK 960
#define SC_LOCAL_MIC_MAX_QUEUED (SC_LOCAL_MIC_RATE / 4 * (int) sizeof(float))
#define SC_LOCAL_MIC_TIMEOUT_US INT64_C(2000000)
#define SC_LOCAL_MIC_CLOCK_WINDOW 51

enum sc_local_microphone_permission
sc_local_microphone_permission(void) {
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusAuthorized: return SC_LOCAL_MIC_AUTHORIZED;
        case AVAuthorizationStatusDenied: return SC_LOCAL_MIC_DENIED;
        case AVAuthorizationStatusRestricted: return SC_LOCAL_MIC_RESTRICTED;
        default: return SC_LOCAL_MIC_NOT_DETERMINED;
    }
}

void
sc_local_microphone_request_permission(
        void (^completion)(enum sc_local_microphone_permission)) {
    enum sc_local_microphone_permission current = sc_local_microphone_permission();
    if (current != SC_LOCAL_MIC_NOT_DETERMINED) {
        completion(current);
        return;
    }
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
        completion(granted ? SC_LOCAL_MIC_AUTHORIZED : sc_local_microphone_permission());
    }];
}

// Keeping backend operations together also permits lifecycle tests with an
// entirely fake device/permission backend. Tests never touch real hardware/TCC.
struct sc_local_microphone_backend {
    enum sc_local_microphone_permission (*permission)(void);
    SDL_AudioStream *(*open)(SDL_AudioDeviceID, const SDL_AudioSpec *,
                             SDL_AudioStreamCallback, void *);
    bool (*resume)(SDL_AudioStream *);
    int (*available)(SDL_AudioStream *);
    int (*read)(SDL_AudioStream *, void *, int);
    bool (*clear)(SDL_AudioStream *);
    bool (*set_callback)(SDL_AudioStream *, SDL_AudioStreamCallback, void *);
    void (*destroy)(SDL_AudioStream *);
    const char *(*error)(void);
    int64_t (*now)(void);
};

static const struct sc_local_microphone_backend real_backend = {
    .permission = sc_local_microphone_permission,
    .open = SDL_OpenAudioDeviceStream,
    .resume = SDL_ResumeAudioStreamDevice,
    .available = SDL_GetAudioStreamAvailable,
    .read = SDL_GetAudioStreamData,
    .clear = SDL_ClearAudioStream,
    .set_callback = SDL_SetAudioStreamPutCallback,
    .destroy = SDL_DestroyAudioStream,
    .error = SDL_GetError,
    .now = av_gettime_relative,
};

struct sc_local_microphone {
    const struct sc_local_microphone_backend *backend;
    SDL_AudioStream *stream;
    struct sc_local_microphone_callbacks callbacks;
    void *userdata;
    atomic_bool stopping;
    atomic_int failed; // 0=no error, 1=publishing error, 2=error ready
    atomic_int_fast64_t last_data_us;
    char error[256];
    int64_t anchor_pts_us;
    uint64_t delivered_samples;
    int64_t phase_us;
    int64_t clock_errors[SC_LOCAL_MIC_CLOCK_WINDOW];
    unsigned clock_error_count;
    unsigned clock_error_next;
    bool has_timestamp;
};

static void
microphone_fail(struct sc_local_microphone *m, const char *message) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&m->failed, &expected, 1)) return;
    snprintf(m->error, sizeof(m->error), "%s", message);
    atomic_store_explicit(&m->failed, 2, memory_order_release);
    if (m->callbacks.on_error && !atomic_load(&m->stopping)) {
        m->callbacks.on_error(m->userdata, m->error);
    }
}

static int64_t
microphone_clock_correction(struct sc_local_microphone *m, int64_t observed_error,
                             unsigned queued_samples) {
    m->clock_errors[m->clock_error_next] = observed_error;
    m->clock_error_next = (m->clock_error_next + 1) % SC_LOCAL_MIC_CLOCK_WINDOW;
    if (m->clock_error_count < SC_LOCAL_MIC_CLOCK_WINDOW) ++m->clock_error_count;
    // Warm up a full robust window so the first delayed callback cannot move
    // the sample timeline. Median rejects isolated scheduler delays; the
    // low-pass and 1000ppm slew cap track slow clock drift without audio holes.
    if (m->clock_error_count < SC_LOCAL_MIC_CLOCK_WINDOW) return m->phase_us;
    int64_t sorted[SC_LOCAL_MIC_CLOCK_WINDOW];
    memcpy(sorted, m->clock_errors, sizeof(sorted));
    for (unsigned i = 1; i < SC_LOCAL_MIC_CLOCK_WINDOW; ++i) {
        int64_t value = sorted[i];
        unsigned j = i;
        while (j && sorted[j - 1] > value) { sorted[j] = sorted[j - 1]; --j; }
        sorted[j] = value;
    }
    int64_t adjustment = (sorted[SC_LOCAL_MIC_CLOCK_WINDOW / 2] - m->phase_us) / 16;
    int64_t limit = (int64_t) queued_samples * 1000 / SC_LOCAL_MIC_RATE;
    if (adjustment > limit) adjustment = limit;
    else if (adjustment < -limit) adjustment = -limit;
    return m->phase_us + adjustment;
}

static void SDLCALL
microphone_data(void *userdata, SDL_AudioStream *stream,
                 int additional_amount, int total_amount) {
    (void) additional_amount;
    (void) total_amount;
    struct sc_local_microphone *m = userdata;
    if (atomic_load(&m->stopping)) return;
    if (atomic_load(&m->failed)) {
        m->backend->clear(stream);
        return;
    }
    int available = m->backend->available(stream);
    if (available < 0) {
        microphone_fail(m, "Local Mic read failed. Stop and check the microphone connection.");
        return;
    }
    if (available > SC_LOCAL_MIC_MAX_QUEUED) {
        microphone_fail(m, "Local Mic fell behind; recording stopped instead of dropping microphone audio.");
        m->backend->clear(stream);
        return;
    }
    if (!available) return;
    int64_t now = m->backend->now();
    int64_t estimated_pts = now - (int64_t) (available / sizeof(float)) * 1000000 / SC_LOCAL_MIC_RATE;
    if (m->has_timestamp) {
        // Use a continuous sample clock with only slow bounded correction;
        // callback scheduling jitter must never become an audio-sized hole.
        int64_t next_pts = m->anchor_pts_us
            + (int64_t) m->delivered_samples * 1000000 / SC_LOCAL_MIC_RATE + m->phase_us;
        int64_t delta = estimated_pts - next_pts;
        if (delta > 250000 || delta < -250000) {
            microphone_fail(m, "Local Mic timing discontinuity; recording stopped to protect synchronization.");
            return;
        }
        int64_t nominal_pts = next_pts - m->phase_us;
        m->phase_us = microphone_clock_correction(m, estimated_pts - nominal_pts,
                                                   available / sizeof(float));
    } else {
        m->anchor_pts_us = estimated_pts;
    }
    float samples[SC_LOCAL_MIC_CHUNK];
    while (available > 0 && !atomic_load(&m->stopping)) {
        int bytes = available < (int) sizeof(samples) ? available : (int) sizeof(samples);
        bytes -= bytes % sizeof(float);
        if (!bytes) break;
        int got = m->backend->read(stream, samples, bytes);
        if (got < 0 || got % sizeof(float)) {
            microphone_fail(m, "Local Mic returned invalid audio data; recording stopped.");
            return;
        }
        if (!got) break;
        size_t count = (size_t) got / sizeof(float);
        int64_t pts = m->anchor_pts_us
            + (int64_t) m->delivered_samples * 1000000 / SC_LOCAL_MIC_RATE + m->phase_us;
        atomic_store(&m->last_data_us, now);
        if (!m->callbacks.on_pcm(m->userdata, samples, count, pts)) {
            microphone_fail(m, "Local Mic audio queue could not accept data; recording stopped.");
            return;
        }
        m->delivered_samples += count;
        m->has_timestamp = true;
        available -= got;
    }
}

static struct sc_local_microphone *
microphone_start_with_backend(bool selected,
    const struct sc_local_microphone_callbacks *callbacks, void *userdata,
    char *error, size_t error_size, const struct sc_local_microphone_backend *backend) {
    if (error_size) error[0] = '\0';
    if (!selected) {
        snprintf(error, error_size, "Local Mic is off.");
        return NULL;
    }
    if (!callbacks || !callbacks->on_pcm) {
        snprintf(error, error_size, "Local Mic has no recording destination.");
        return NULL;
    }
    if (backend->permission() != SC_LOCAL_MIC_AUTHORIZED) {
        snprintf(error, error_size, "Local Mic permission is not granted. Enable Local Mic to request access.");
        return NULL;
    }
    struct sc_local_microphone *m = calloc(1, sizeof(*m));
    if (!m) {
        snprintf(error, error_size, "Cannot allocate Local Mic capture.");
        return NULL;
    }
    m->backend = backend;
    m->callbacks = *callbacks;
    m->userdata = userdata;
    atomic_init(&m->stopping, false);
    atomic_init(&m->failed, 0);
    atomic_init(&m->last_data_us, backend->now());
    SDL_AudioSpec spec = {.format = SDL_AUDIO_F32, .channels = 1, .freq = SC_LOCAL_MIC_RATE};
    m->stream = backend->open(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, microphone_data, m);
    if (!m->stream || !backend->resume(m->stream)) {
        snprintf(error, error_size, "Local Mic could not open: %s", backend->error());
        sc_local_microphone_stop(m);
        return NULL;
    }
    if (atomic_load_explicit(&m->failed, memory_order_acquire)) {
        // Some backends deliver immediately from resume(). Do not announce a
        // usable source if its initial callback already failed.
        snprintf(error, error_size, "Local Mic failed during startup.");
        sc_local_microphone_stop(m);
        return NULL;
    }
    return m;
}

struct sc_local_microphone *
sc_local_microphone_start(bool selected,
    const struct sc_local_microphone_callbacks *callbacks, void *userdata,
    char *error, size_t error_size) {
    return microphone_start_with_backend(selected, callbacks, userdata,
                                         error, error_size, &real_backend);
}

bool
sc_local_microphone_poll(struct sc_local_microphone *m, char *error, size_t size) {
    if (!m) {
        snprintf(error, size, "Local Mic is not open.");
        return false;
    }
    if (!atomic_load(&m->failed)) {
        if (m->backend->permission() != SC_LOCAL_MIC_AUTHORIZED) {
            microphone_fail(m, "Local Mic permission was revoked; recording stopped.");
        } else if (m->backend->now() - atomic_load(&m->last_data_us) > SC_LOCAL_MIC_TIMEOUT_US) {
            microphone_fail(m, "Local Mic stopped delivering audio. Check its connection and try again.");
        }
    }
    int failed = atomic_load_explicit(&m->failed, memory_order_acquire);
    if (failed) {
        snprintf(error, size, "%s", failed == 2 ? m->error : "Local Mic failed; recording stopped.");
        return false;
    }
    if (size) error[0] = '\0';
    return true;
}

void
sc_local_microphone_stop(struct sc_local_microphone *m) {
    if (!m) return;
    atomic_store(&m->stopping, true);
    if (m->stream) {
        // SDL documents that replacing this callback acquires its stream lock
        // and waits for an in-progress callback before returning.
        m->backend->set_callback(m->stream, NULL, NULL);
        m->backend->destroy(m->stream);
    }
    free(m);
}
