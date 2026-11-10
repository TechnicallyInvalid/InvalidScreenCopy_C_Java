// Lifecycle/privacy integration with the real recorder/mixer and fake captures.
// No AudioRecord, SDL recording device, permission API, window or phone is used.
#include "recording.h"
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include <libavutil/time.h>

struct mock_backend {
    struct sc_recording *recording;
    sc_mutex mutex;
    sc_thread producer;
    bool ending;
    bool android_on;
    bool local_on;
    bool fail_local;
    bool fail_android;
    bool fail_stop;
    bool error_on_stop;
    unsigned stop_delay_ms;
    unsigned android_starts, android_stops, local_starts, local_stops, queries;
    uint32_t session;
    int64_t latest_pts;
};

static bool
android(void *userdata, bool enabled, uint32_t session) {
    struct mock_backend *m = userdata;
    sc_mutex_lock(&m->mutex);
    m->session = session;
    if (enabled) ++m->android_starts;
    else ++m->android_stops;
    bool failed = enabled ? m->fail_android : m->fail_stop;
    bool source_error = !enabled && m->error_on_stop;
    m->android_on = enabled && !failed;
    sc_mutex_unlock(&m->mutex);
    if (enabled) sc_recording_android_status(m->recording, session, failed ? 2 : 1,
        failed ? "Synthetic Android permission denied." : "Ready");
    if (source_error) sc_recording_android_status(m->recording, session, 2,
        "Synthetic Android capture failed during Stop.");
    return enabled || !failed;
}

static bool
query(void *userdata, uint32_t session, uint32_t sequence) {
    (void) sequence;
    struct mock_backend *m = userdata;
    sc_mutex_lock(&m->mutex);
    m->session = session;
    ++m->queries;
    sc_mutex_unlock(&m->mutex);
    return true;
}

static bool
clock_ready(void *userdata, uint32_t session, char *error, size_t size) {
    (void) session; (void) error; (void) size;
    struct mock_backend *m = userdata;
    sc_mutex_lock(&m->mutex);
    bool ready = m->queries >= 5;
    sc_mutex_unlock(&m->mutex);
    return ready;
}

static bool
local_start(void *userdata, uint32_t session, char *error, size_t size) {
    struct mock_backend *m = userdata;
    sc_mutex_lock(&m->mutex);
    m->session = session;
    ++m->local_starts;
    bool failed = m->fail_local;
    m->local_on = !failed;
    sc_mutex_unlock(&m->mutex);
    if (failed) snprintf(error, size, "Synthetic local permission denied.");
    return !failed;
}

static bool
local_poll(void *userdata, char *error, size_t size) {
    (void) userdata; (void) error; (void) size;
    return true;
}

static void
local_stop(void *userdata) {
    struct mock_backend *m = userdata;
    sc_mutex_lock(&m->mutex);
    ++m->local_stops;
    m->local_on = false;
    unsigned delay = m->stop_delay_ms;
    sc_mutex_unlock(&m->mutex);
    if (delay) SDL_Delay(delay); // Capture is already stopped; simulate cleanup.
}

static const struct sc_recording_microphone_ops ops = {
    .set_android = android, .query_clock = query, .clock_ready = clock_ready,
    .start_local = local_start, .poll_local = local_poll, .stop_local = local_stop,
};

static int
produce(void *userdata) {
    struct mock_backend *m = userdata;
    AVFrame *device = av_frame_alloc();
    assert(device);
    device->format = AV_SAMPLE_FMT_FLT;
    device->sample_rate = 48000;
    device->nb_samples = 960;
    device->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
    assert(!av_frame_get_buffer(device, 0));
    float local[960];
    int16_t mobile[960];
    int64_t block = 0;
    int64_t next_deadline = av_gettime_relative();
    for (;;) {
        // Pace against an absolute clock. Relative sleeps would accumulate
        // scheduler overhead and make the fake microphones artificially slow.
        int64_t remaining = next_deadline - av_gettime_relative();
        if (remaining > 0) SDL_DelayNS((uint64_t) remaining * 1000);
        sc_mutex_lock(&m->mutex);
        bool ending = m->ending, android_on = m->android_on, local_on = m->local_on;
        uint32_t session = m->session;
        sc_mutex_unlock(&m->mutex);
        if (ending) break;
        assert(!av_frame_make_writable(device));
        float *pcm = (float *) device->data[0];
        for (int i = 0; i < 960; ++i) {
            double t = (block * 960 + i) / 48000.0;
            pcm[2*i] = pcm[2*i+1] = .15f * sin(2 * M_PI * 440 * t);
            local[i] = .15f * sin(2 * M_PI * 1200 * t);
            mobile[i] = (int16_t) (5000 * sin(2 * M_PI * 800 * t));
        }
        int64_t pts = 1000000 + block * 20000;
        sc_mutex_lock(&m->mutex);
        m->latest_pts = pts;
        sc_mutex_unlock(&m->mutex);
        if (android_on) sc_recording_android_pcm(m->recording, session, pts,
            (const uint8_t *) mobile, sizeof(mobile));
        if (local_on) sc_recording_local_pcm(m->recording, session, pts, local, 960);
        device->pts = pts;
        assert(m->recording->audio_frame_sink.ops->push(&m->recording->audio_frame_sink, device) == SC_SINK_OK);
        ++block;
        next_deadline += 20000;
    }
    av_frame_free(&device);
    return 0;
}

static void
configure(struct sc_recording *r) {
    AVCodecParameters *params = avcodec_parameters_alloc();
    assert(params);
    params->codec_type = AVMEDIA_TYPE_VIDEO;
    params->codec_id = AV_CODEC_ID_H264;
    params->width = 320;
    params->height = 240;
    assert(r->video.sink.ops->open(&r->video.sink, params, NULL));
    static const uint8_t avcc[] = {
        1,100,0,13,255,225,0,23,103,100,0,13,172,178,2,131,246,2,32,0,0,3,
        0,32,0,0,7,129,226,133,73,1,0,6,104,235,195,203,34,192,253,248,248,0,
    };
    AVPacket config = {.data = (uint8_t *) avcc, .size = sizeof(avcc), .pts = AV_NOPTS_VALUE};
    assert(r->video.sink.ops->push(&r->video.sink, &config) == SC_SINK_OK);
    params->codec_type = AVMEDIA_TYPE_AUDIO;
    params->codec_id = AV_CODEC_ID_AAC;
    params->sample_rate = 48000;
    params->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
    assert(r->audio.sink.ops->open(&r->audio.sink, params, NULL));
    static uint8_t asc[] = {0x11, 0x90};
    config.data = asc;
    config.size = sizeof(asc);
    assert(r->audio.sink.ops->push(&r->audio.sink, &config) == SC_SINK_OK);
    avcodec_parameters_free(&params);
    assert(r->audio_frame_sink.ops->open(&r->audio_frame_sink, NULL, NULL));
}

static void
reset_counts(struct mock_backend *m) {
    sc_mutex_lock(&m->mutex);
    assert(!m->android_on && !m->local_on);
    m->android_starts = m->android_stops = m->local_starts = m->local_stops = m->queries = 0;
    m->fail_android = m->fail_local = m->fail_stop = false;
    m->error_on_stop = false;
    m->stop_delay_ms = 0;
    sc_mutex_unlock(&m->mutex);
}

static void
assert_counts(struct mock_backend *m, unsigned android_count, unsigned local_count) {
    sc_mutex_lock(&m->mutex);
    assert(!m->android_on && !m->local_on);
    assert(m->android_starts == android_count && m->android_stops == android_count);
    assert(m->local_starts == local_count && m->local_stops == local_count);
    if (!(android_count || local_count)) assert(!m->queries);
    sc_mutex_unlock(&m->mutex);
}

static void
assert_empty(const char *directory) {
    DIR *dir = opendir(directory);
    assert(dir);
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        assert(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
    }
    closedir(dir);
}

static void
delayed_stop_clip(const char *fixture, const char *directory, unsigned microphones) {
    struct sc_recording r;
    assert(sc_recording_init(&r, true, SC_ORIENTATION_0));
    configure(&r);
    struct mock_backend mock = {.recording = &r,
        .stop_delay_ms = (microphones & SC_RECORDING_LOCAL_MIC) ? 400 : 0};
    assert(sc_mutex_init(&mock.mutex));
    sc_recording_set_microphone_backend(&r, &ops, &mock, true);
    assert(sc_recording_set_microphones(&r, microphones & SC_RECORDING_ANDROID_MIC,
                                          microphones & SC_RECORDING_LOCAL_MIC));
    assert(sc_thread_create(&mock.producer, produce, "test-mic-av", &mock));
    assert(sc_recording_start(&r, directory, 1794100001999 + microphones));
    AVFormatContext *input = NULL;
    assert(!avformat_open_input(&input, fixture, NULL, NULL));
    assert(avformat_find_stream_info(input, NULL) >= 0);
    AVPacket *packet = av_packet_alloc();
    assert(packet);
    bool keyframe = false;
    while (av_read_frame(input, packet) >= 0) {
        if (input->streams[packet->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
                && (packet->flags & AV_PKT_FLAG_KEY)) {
            sc_mutex_lock(&mock.mutex);
            packet->pts = packet->dts = mock.latest_pts;
            sc_mutex_unlock(&mock.mutex);
            packet->duration = 0;
            assert(r.video.sink.ops->push(&r.video.sink, packet) == SC_SINK_OK);
            keyframe = true;
            break;
        }
        av_packet_unref(packet);
    }
    assert(keyframe);
    av_packet_free(&packet);
    avformat_close_input(&input);
    SDL_Delay(500); // Synthetic video holds one real keyframe while tones run.
    int64_t start_stop = av_gettime_relative();
    sc_recording_request_stop(&r);
    sc_mutex_lock(&r.mutex);
    int64_t requested_duration = r.stop_pts - r.first_pts;
    sc_mutex_unlock(&r.mutex);
    if (mock.stop_delay_ms) {
        assert(!sc_recording_wait_microphones_stopped(&r, sc_tick_now() + SC_TICK_FROM_MS(20)));
    }
    assert(sc_recording_wait_microphones_stopped(&r, sc_tick_now() + SC_TICK_FROM_SEC(3)));
    // Normal teardown EOF after Stop must not turn a valid clip into an error.
    r.audio_frame_sink.ops->close(&r.audio_frame_sink);
    r.video.sink.ops->close(&r.video.sink);
    sc_recording_stop(&r);
    if (mock.stop_delay_ms) assert(av_gettime_relative() - start_stop >= 350000);
    assert(!strncmp(r.message, "Saved: ", 7));
    AVFormatContext *output = NULL;
    assert(!avformat_open_input(&output, r.message + 7, NULL, NULL));
    assert(avformat_find_stream_info(output, NULL) >= 0);
    AVStream *video = NULL, *audio = NULL;
    for (unsigned i = 0; i < output->nb_streams; ++i) {
        if (output->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) video = output->streams[i];
        if (output->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio = output->streams[i];
    }
    assert(video && audio);
    int64_t video_duration = av_rescale_q(video->duration, video->time_base, AV_TIME_BASE_Q);
    int64_t audio_duration = av_rescale_q(audio->duration, audio->time_base, AV_TIME_BASE_Q);
    assert(llabs(video_duration - requested_duration) < 3000);
    assert(llabs(audio_duration - video_duration) < 50000);
    printf("PASS: mixed A/V mask=%u clip %s; frozen Stop duration=%lldus, video=%lldus audio=%lldus; cleanup=%ums excluded\n",
        microphones, r.message + 7, (long long) requested_duration,
        (long long) video_duration, (long long) audio_duration, mock.stop_delay_ms);
    avformat_close_input(&output);
    sc_mutex_lock(&mock.mutex);
    mock.ending = true;
    sc_mutex_unlock(&mock.mutex);
    sc_thread_join(&mock.producer, NULL);
    sc_recording_destroy(&r);
    assert_counts(&mock, !!(microphones & SC_RECORDING_ANDROID_MIC),
                        !!(microphones & SC_RECORDING_LOCAL_MIC));
    sc_mutex_destroy(&mock.mutex);
}

int
main(int argc, char **argv) {
    assert(argc == 1 || argc == 3);
    assert(SDL_Init(SDL_INIT_EVENTS));
    struct sc_recording r;
    assert(sc_recording_init(&r, true, SC_ORIENTATION_0));
    bool android_selected, local_selected;
    sc_recording_get_microphones(&r, &android_selected, &local_selected);
    assert(!android_selected && !local_selected);
    struct mock_backend mock = {.recording = &r};
    assert(sc_mutex_init(&mock.mutex));
    sc_recording_set_microphone_backend(&r, &ops, &mock, true);
    configure(&r);
    assert(sc_thread_create(&mock.producer, produce, "test-mic-pcm", &mock));
    char directory[] = "/tmp/isc-recording-mics-XXXXXX";
    assert(mkdtemp(directory));

    // Merely selecting toggles never invokes any capture or clock operation.
    // Repeat both-mic selection to exercise session retirement between clips.
    const unsigned masks[] = {0, SC_RECORDING_ANDROID_MIC, SC_RECORDING_LOCAL_MIC,
        SC_RECORDING_ANDROID_MIC | SC_RECORDING_LOCAL_MIC,
        SC_RECORDING_ANDROID_MIC | SC_RECORDING_LOCAL_MIC, 0};
    for (unsigned i = 0; i < sizeof(masks)/sizeof(*masks); ++i) {
        reset_counts(&mock);
        assert(sc_recording_set_microphones(&r, masks[i] & SC_RECORDING_ANDROID_MIC,
            masks[i] & SC_RECORDING_LOCAL_MIC));
        assert_counts(&mock, 0, 0);
        assert(sc_recording_start(&r, directory, 1794100000000 + i));
        assert(!sc_recording_set_microphones(&r, false, false));
        assert(sc_recording_take_keyframe_request(&r));
        sc_recording_stop(&r);
        assert_counts(&mock, !!(masks[i] & SC_RECORDING_ANDROID_MIC),
                              !!(masks[i] & SC_RECORDING_LOCAL_MIC));
        assert(r.audio.config && r.audio.config->size == 2); // passthrough metadata survives a mixed clip
        assert_empty(directory); // no video frame: remove only our empty clip
        // Late packets/errors from a finished session must not change status.
        char old[sizeof(r.message)];
        snprintf(old, sizeof(old), "%s", r.message);
        sc_recording_android_status(&r, mock.session, 2, "Stale error");
        assert(!strcmp(old, r.message));
    }

    reset_counts(&mock);
    mock.fail_local = true;
    assert(sc_recording_set_microphones(&r, false, true));
    assert(!sc_recording_start(&r, directory, 1794100000100));
    sc_recording_stop(&r);
    assert(strstr(r.message, "Synthetic local permission denied"));
    assert_counts(&mock, 0, 1);
    assert_empty(directory);

    reset_counts(&mock);
    mock.fail_android = true;
    assert(sc_recording_set_microphones(&r, true, false));
    assert(!sc_recording_start(&r, directory, 1794100000101));
    sc_recording_stop(&r);
    assert(strstr(r.message, "Synthetic Android permission denied"));
    assert_counts(&mock, 1, 0);
    assert_empty(directory);

    reset_counts(&mock);
    mock.error_on_stop = true;
    assert(sc_recording_start(&r, directory, 1794100000107));
    sc_recording_stop(&r);
    assert(strstr(r.message, "capture failed during Stop"));
    assert_counts(&mock, 1, 0);
    assert_empty(directory);

    reset_counts(&mock);
    mock.fail_stop = true;
    assert(sc_recording_start(&r, directory, 1794100000102));
    sc_recording_stop(&r);
    assert(strstr(r.message, "control connection closed"));
    assert_counts(&mock, 1, 0);
    assert_empty(directory);

    reset_counts(&mock);
    assert(sc_recording_set_microphones(&r, true, true));
    assert(sc_recording_start(&r, directory, 1794100000103));
    r.audio_frame_sink.ops->close(&r.audio_frame_sink);
    sc_recording_stop(&r);
    assert(strstr(r.message, "device audio ended"));
    assert_counts(&mock, 1, 1);
    assert_empty(directory);

    // Device output is mandatory even when neither microphone is selected.
    // A missing or closed output stream must never become a silent video-only
    // success, while the explicit no-audio legacy tests remain separate.
    reset_counts(&mock);
    assert(sc_recording_set_microphones(&r, false, false));
    r.audio.sink.ops->disable(&r.audio.sink);
    bool ready, active;
    char status[1024];
    sc_recording_poll(&r, &ready, &active, status, sizeof(status));
    assert(!ready && !active && strstr(status, "device audio is unavailable"));
    assert(!sc_recording_request_start(&r, directory, 1794100000104));
    configure(&r);
    assert(sc_recording_start(&r, directory, 1794100000105));
    r.audio.sink.ops->close(&r.audio.sink);
    sc_recording_stop(&r);
    assert(strstr(r.message, "device audio ended unexpectedly"));
    assert_counts(&mock, 0, 0);
    assert_empty(directory);

    reset_counts(&mock);
    configure(&r);
    assert(sc_recording_set_microphones(&r, true, true));
    assert(sc_recording_start(&r, directory, 1794100000106));
    sc_mutex_lock(&mock.mutex);
    mock.ending = true;
    sc_mutex_unlock(&mock.mutex);
    sc_thread_join(&mock.producer, NULL);
    sc_recording_destroy(&r); // shutdown itself must stop both selected captures
    assert_counts(&mock, 1, 1);
    assert_empty(directory);
    assert(!rmdir(directory));
    sc_mutex_destroy(&mock.mutex);
    if (argc == 3) {
        delayed_stop_clip(argv[1], argv[2], SC_RECORDING_ANDROID_MIC);
        delayed_stop_clip(argv[1], argv[2], SC_RECORDING_LOCAL_MIC);
        delayed_stop_clip(argv[1], argv[2], SC_RECORDING_ANDROID_MIC | SC_RECORDING_LOCAL_MIC);
    }
    SDL_Quit();
    return 0;
}
