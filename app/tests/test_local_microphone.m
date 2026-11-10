// No microphone, SDL device, permission lookup/request or UI is used. Exercise
// the actual adapter against a completely fake capture/permission backend.
#include <assert.h>
#include "../src/local_microphone.m"

static struct {
    enum sc_local_microphone_permission permission;
    unsigned permission_queries, opens, resumes, detaches, closes, clears;
    unsigned deliveries, errors;
    bool open_fails, resume_fails, resume_emits, read_fails, reject_pcm;
    int available;
    int64_t now, first_pts, last_pts;
    size_t samples;
    SDL_AudioStreamCallback callback;
    void *userdata;
    char message[256];
} fake;

static enum sc_local_microphone_permission permission(void) {
    ++fake.permission_queries;
    return fake.permission;
}
static SDL_AudioStream *open_device(SDL_AudioDeviceID id, const SDL_AudioSpec *spec,
                                   SDL_AudioStreamCallback callback, void *userdata) {
    assert(id == SDL_AUDIO_DEVICE_DEFAULT_RECORDING);
    assert(spec->channels == 1 && spec->freq == 48000 && spec->format == SDL_AUDIO_F32);
    ++fake.opens;
    if (fake.open_fails) return NULL;
    fake.callback = callback;
    fake.userdata = userdata;
    return (SDL_AudioStream *) &fake;
}
static bool resume_device(SDL_AudioStream *stream) {
    assert(stream == (SDL_AudioStream *) &fake);
    ++fake.resumes;
    if (fake.resume_emits) {
        fake.available = 960 * sizeof(float);
        fake.callback(fake.userdata, stream, fake.available, fake.available);
    }
    return !fake.resume_fails;
}
static int available(SDL_AudioStream *stream) {
    assert(stream == (SDL_AudioStream *) &fake);
    return fake.available;
}
static int read_samples(SDL_AudioStream *stream, void *data, int bytes) {
    assert(stream == (SDL_AudioStream *) &fake);
    if (fake.read_fails) return -1;
    assert(bytes <= fake.available && bytes % sizeof(float) == 0);
    float *out = data;
    for (unsigned i = 0; i < bytes / sizeof(float); ++i) out[i] = 0.25f;
    fake.available -= bytes;
    return bytes;
}
static bool clear(SDL_AudioStream *stream) {
    assert(stream == (SDL_AudioStream *) &fake);
    fake.available = 0;
    ++fake.clears;
    return true;
}
static bool detach(SDL_AudioStream *stream, SDL_AudioStreamCallback callback, void *userdata) {
    assert(stream == (SDL_AudioStream *) &fake && !callback && !userdata);
    ++fake.detaches;
    fake.callback = NULL;
    fake.userdata = NULL;
    return true;
}
static void close_device(SDL_AudioStream *stream) {
    assert(stream == (SDL_AudioStream *) &fake);
    assert(fake.detaches && !fake.callback && !fake.userdata);
    ++fake.closes;
}
static const char *last_error(void) { return "synthetic device failure"; }
static int64_t now(void) { return fake.now; }
static const struct sc_local_microphone_backend backend = {
    .permission = permission,
    .open = open_device,
    .resume = resume_device,
    .available = available,
    .read = read_samples,
    .clear = clear,
    .set_callback = detach,
    .destroy = close_device,
    .error = last_error,
    .now = now,
};

static bool delivered(void *userdata, const float *samples, size_t count, int64_t pts) {
    assert(userdata == &fake && count > 0 && count <= SC_LOCAL_MIC_CHUNK);
    assert(samples[0] == 0.25f && samples[count - 1] == 0.25f);
    if (!fake.deliveries) fake.first_pts = pts;
    else assert(pts >= fake.last_pts);
    ++fake.deliveries;
    fake.last_pts = pts;
    fake.samples += count;
    return !fake.reject_pcm;
}
static void failed(void *userdata, const char *message) {
    assert(userdata == &fake);
    ++fake.errors;
    snprintf(fake.message, sizeof(fake.message), "%s", message);
}
static const struct sc_local_microphone_callbacks callbacks = {
    .on_pcm = delivered, .on_error = failed,
};
static void reset(void) {
    memset(&fake, 0, sizeof(fake));
    fake.permission = SC_LOCAL_MIC_AUTHORIZED;
    fake.now = 1000000;
}
static void emit(int frames) {
    assert(fake.callback);
    fake.available = frames * sizeof(float);
    fake.callback(fake.userdata, (SDL_AudioStream *) &fake, fake.available, fake.available);
}
static struct sc_local_microphone *start(bool selected, char *error) {
    return microphone_start_with_backend(selected, &callbacks, &fake, error, 256, &backend);
}

int main(void) {
    char error[256];
    reset();
    assert(!start(false, error));
    assert(!fake.permission_queries && !fake.opens && !fake.resumes);
    assert(strstr(error, "off"));
    for (int p = SC_LOCAL_MIC_NOT_DETERMINED; p <= SC_LOCAL_MIC_RESTRICTED; ++p) {
        if (p == SC_LOCAL_MIC_AUTHORIZED) continue;
        reset(); fake.permission = p;
        assert(!start(true, error));
        assert(fake.permission_queries == 1 && !fake.opens);
        assert(strstr(error, "permission"));
    }
    reset(); fake.open_fails = true;
    assert(!start(true, error));
    assert(fake.opens == 1 && !fake.resumes && !fake.closes);
    assert(strstr(error, "could not open"));
    reset(); fake.resume_fails = true;
    assert(!start(true, error));
    assert(fake.opens == 1 && fake.resumes == 1 && fake.detaches == 1 && fake.closes == 1);
    reset(); fake.resume_emits = true; fake.reject_pcm = true;
    assert(!start(true, error));
    assert(fake.errors == 1 && fake.detaches == 1 && fake.closes == 1);
    assert(strstr(error, "startup"));

    // Repeated recordings never leave a recording device or callback alive.
    for (unsigned clip = 0; clip < 4; ++clip) {
        reset();
        struct sc_local_microphone *m = start(true, error);
        assert(m && fake.opens == 1 && fake.resumes == 1 && !fake.deliveries);
        emit(1920); // two queued 20ms chunks, first sample 40ms before callback
        assert(fake.first_pts == 960000 && fake.last_pts == 980000 && fake.samples == 1920);
        fake.now += 20000; emit(960);
        assert(fake.last_pts == 1000000 && fake.samples == 2880);
        assert(sc_local_microphone_poll(m, error, sizeof(error)));
        sc_local_microphone_stop(m);
        assert(fake.closes == 1 && fake.detaches == 1 && !fake.callback);
    }

    // An irregular scheduler does not imply gaps in hardware samples. Even
    // alternating 1ms/39ms callbacks retain a continuous 48k sample timeline;
    // slow clock correction never inserts a callback-sized gap.
    reset();
    struct sc_local_microphone *m = start(true, error);
    emit(960);
    for (unsigned i = 0; i < 100; ++i) {
        fake.now += i % 2 ? 39000 : 1000;
        emit(960);
        int64_t expected = 980000 + (int64_t) (i + 1) * 20000;
        assert(llabs(fake.last_pts - expected) < 1000);
        assert(!fake.errors);
    }
    fake.now += 60000; emit(2880);
    assert(llabs(fake.last_pts - (980000 + 103 * INT64_C(20000))) < 1000);
    assert(!fake.errors);
    sc_local_microphone_stop(m);

    // Ten minutes of either sign of 200ppm hardware-clock drift with delayed
    // callbacks. Correction is bounded to <=1000ppm, all samples are delivered,
    // timestamps are monotonic and offset stays below 3ms, not minutes of drift.
    for (int ppm = -200; ppm <= 200; ppm += 400) {
        reset(); m = start(true, error); emit(960);
        int64_t previous_pts = fake.last_pts;
        for (int64_t block = 1; block <= 30000; ++block) {
            int64_t nominal = block * 20000;
            int64_t clock_drift = nominal * ppm / 1000000;
            int64_t jitter = block % 5 == 1 ? 10000 : block % 5 == 3 ? 20000 : 0;
            fake.now = 1000000 + nominal + clock_drift + jitter;
            emit(960);
            assert(!fake.errors);
            int64_t delta = fake.last_pts - previous_pts;
            assert(delta >= 19980 && delta <= 20020);
            int64_t offset = fake.last_pts - (980000 + nominal + clock_drift);
            assert(offset > -3000 && offset < 3000);
            previous_pts = fake.last_pts;
        }
        assert(fake.samples == 30001 * 960);
        sc_local_microphone_stop(m);
    }

    reset();
    m = start(true, error);
    fake.now += SC_LOCAL_MIC_TIMEOUT_US + 1;
    assert(!sc_local_microphone_poll(m, error, sizeof(error)));
    assert(strstr(error, "stopped delivering") && fake.errors == 1);
    sc_local_microphone_stop(m);

    reset(); m = start(true, error);
    fake.permission = SC_LOCAL_MIC_DENIED;
    assert(!sc_local_microphone_poll(m, error, sizeof(error)));
    assert(strstr(error, "revoked") && fake.errors == 1);
    sc_local_microphone_stop(m);

    reset(); m = start(true, error);
    emit(SC_LOCAL_MIC_MAX_QUEUED / sizeof(float) + 1);
    assert(fake.errors == 1 && fake.clears == 1 && !fake.deliveries);
    assert(!sc_local_microphone_poll(m, error, sizeof(error)));
    emit(960); // an errored source stays bounded while the owner stops it
    assert(fake.errors == 1 && fake.clears == 2 && !fake.available);
    sc_local_microphone_stop(m);

    reset(); m = start(true, error);
    fake.reject_pcm = true; emit(960);
    assert(fake.errors == 1 && strstr(fake.message, "queue"));
    sc_local_microphone_stop(m);

    reset(); m = start(true, error);
    fake.read_fails = true; emit(960);
    assert(fake.errors == 1 && strstr(fake.message, "invalid audio"));
    sc_local_microphone_stop(m);

    reset(); m = start(true, error);
    emit(960); fake.now += 1000000; emit(960);
    assert(fake.errors == 1 && strstr(fake.message, "timing discontinuity"));
    sc_local_microphone_stop(m);
    sc_local_microphone_stop(NULL);
    puts("PASS: Local Mic off/denied never opens; repeated clips; queued-sample timestamps; jitter and 10-minute +/-200ppm drift; failed open, revoke, disconnect, bounded backlog, timing and sink errors");
    return 0;
}
