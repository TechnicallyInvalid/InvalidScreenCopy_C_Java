// Deterministic blocked-folder / blocked-close tests of the real recording
// lifecycle. Interpose only file creation; muxing, sinks and worker are real.
#include "recording.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <SDL3/SDL.h>

static FILE *gated_create_file(const char *directory, int64_t unix_ms, char **path);
#define sc_recording_create_file gated_create_file
#include "../src/recording.c"
#undef sc_recording_create_file

static struct {
    sc_mutex mutex;
    sc_cond cond;
    bool block_open;
    bool open_entered;
    bool release_open;
    bool block_close;
    bool close_entered;
    bool release_close;
    bool fail_open;
    int64_t timestamp;
} gate;

static void
wait_gate(bool *entered) {
    sc_mutex_lock(&gate.mutex);
    sc_tick deadline = sc_tick_now() + SC_TICK_FROM_SEC(5);
    while (!*entered) {
        assert(sc_cond_timedwait(&gate.cond, &gate.mutex, deadline));
    }
    sc_mutex_unlock(&gate.mutex);
}

static void
release_gate(bool *release) {
    sc_mutex_lock(&gate.mutex);
    *release = true;
    sc_cond_broadcast(&gate.cond);
    sc_mutex_unlock(&gate.mutex);
}

static int
forward_write(void *cookie, const char *bytes, int size) {
    return (int) fwrite(bytes, 1, size, cookie);
}

static int
blocked_close(void *cookie) {
    sc_mutex_lock(&gate.mutex);
    gate.close_entered = true;
    sc_cond_broadcast(&gate.cond);
    while (!gate.release_close) {
        sc_cond_wait(&gate.cond, &gate.mutex);
    }
    sc_mutex_unlock(&gate.mutex);
    return fclose(cookie);
}

static FILE *
gated_create_file(const char *directory, int64_t unix_ms, char **path) {
    sc_mutex_lock(&gate.mutex);
    gate.timestamp = unix_ms;
    gate.open_entered = true;
    sc_cond_broadcast(&gate.cond);
    while (gate.block_open && !gate.release_open) {
        sc_cond_wait(&gate.cond, &gate.mutex);
    }
    bool fail_open = gate.fail_open;
    bool block_close = gate.block_close;
    sc_mutex_unlock(&gate.mutex);
    if (fail_open) {
        errno = EACCES;
        return NULL;
    }
    FILE *file = sc_recording_create_file(directory, unix_ms, path);
    if (!file || !block_close) return file;
    FILE *wrapped = funopen(file, NULL, forward_write, NULL, blocked_close);
    assert(wrapped);
    return wrapped;
}

static void
reset_gate(bool block_open, bool block_close) {
    gate.block_open = block_open;
    gate.open_entered = false;
    gate.release_open = false;
    gate.block_close = block_close;
    gate.close_entered = false;
    gate.release_close = false;
    gate.fail_open = false;
}

static void
configure_video(struct sc_recording *r) {
    AVCodecParameters *params = avcodec_parameters_alloc();
    assert(params);
    params->codec_type = AVMEDIA_TYPE_VIDEO;
    params->codec_id = AV_CODEC_ID_H264;
    params->width = 320;
    params->height = 240;
    assert(r->video.sink.ops->open(&r->video.sink, params, NULL));
    avcodec_parameters_free(&params);
    // AVC decoder configuration from our generated 320x240 H.264 fixture.
    static const uint8_t avcc[] = {
        0x01,0x64,0x00,0x0d,0xff,0xe1,0x00,0x17,0x67,0x64,0x00,
        0x0d,0xac,0xb2,0x02,0x83,0xf6,0x02,0x20,0x00,0x00,0x03,
        0x00,0x20,0x00,0x00,0x07,0x81,0xe2,0x85,0x49,0x01,0x00,
        0x06,0x68,0xeb,0xc3,0xcb,0x22,0xc0,0xfd,0xf8,0xf8,0x00,
    };
    AVPacket *config = av_packet_alloc();
    assert(config && !av_new_packet(config, sizeof(avcc)));
    memcpy(config->data, avcc, sizeof(avcc));
    config->pts = AV_NOPTS_VALUE;
    assert(r->video.sink.ops->push(&r->video.sink, config) == SC_SINK_OK);
    av_packet_free(&config);
}

static void
assert_responsive(struct sc_recording *r, bool stoppable) {
    bool ready, active;
    char message[1024];
    sc_recording_poll(r, &ready, &active, message, sizeof(message));
    assert(active);
    assert(sc_recording_can_stop(r) == stoppable);
    // Both demuxer inputs remain usable while a folder or file close is stuck.
    AVPacket packet = {.pts = 0, .dts = 0};
    assert(r->video.sink.ops->push(&r->video.sink, &packet) == SC_SINK_OK);
    assert(r->audio.sink.ops->push(&r->audio.sink, &packet) == SC_SINK_OK);
}

static void
wait_idle(struct sc_recording *r) {
    sc_mutex_lock(&r->mutex);
    sc_tick deadline = sc_tick_now() + SC_TICK_FROM_SEC(5);
    while (r->busy) {
        assert(sc_cond_timedwait(&r->cond, &r->mutex, deadline));
    }
    sc_mutex_unlock(&r->mutex);
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

int
main(void) {
    assert(SDL_Init(SDL_INIT_EVENTS));
    assert(sc_mutex_init(&gate.mutex));
    assert(sc_cond_init(&gate.cond));
    char directory[] = "/tmp/invalid-recording-async-XXXXXX";
    assert(mkdtemp(directory));
    struct sc_recording r;
    assert(sc_recording_init(&r, false, SC_ORIENTATION_0));
    configure_video(&r);

    // A stalled destination must not block polling, stream delivery or Stop.
    reset_gate(true, false);
    assert(sc_recording_request_start(&r, directory, 1794100000123));
    wait_gate(&gate.open_entered);
    assert_responsive(&r, true);
    assert(!sc_recording_take_keyframe_request(&r));
    assert(!sc_recording_request_start(&r, directory, 1794100000124));
    sc_recording_request_stop(&r);
    assert_responsive(&r, false);
    release_gate(&gate.release_open);
    sc_recording_stop(&r);
    assert(gate.timestamp == 1794100000123);
    assert_empty(directory);

    // Metadata copied for a pending start cannot be used after stream change.
    reset_gate(true, false);
    assert(sc_recording_request_start(&r, directory, 1794100000456));
    wait_gate(&gate.open_entered);
    struct sc_stream_session session = {.video = {.width = 640, .height = 480}};
    assert(r.video.sink.ops->push_session(&r.video.sink, &session) == SC_SINK_OK);
    release_gate(&gate.release_open);
    wait_idle(&r);
    assert(strstr(r.message, "Stream changed"));
    assert(!sc_recording_take_keyframe_request(&r));
    assert_empty(directory);

    // An error from an asynchronous file open becomes status, not a stream error.
    configure_video(&r);
    reset_gate(true, false);
    gate.fail_open = true;
    assert(sc_recording_request_start(&r, directory, 1794100000789));
    wait_gate(&gate.open_entered);
    assert_responsive(&r, true);
    release_gate(&gate.release_open);
    wait_idle(&r);
    assert(strstr(r.message, "Cannot create recording"));

    // Finalization may block indefinitely on a filesystem, but the UI and
    // demuxers do not wait for it. A new recording stays disabled until cleanup.
    reset_gate(false, true);
    assert(sc_recording_start(&r, directory, 1794100000999));
    assert(sc_recording_take_keyframe_request(&r));
    assert(!sc_recording_take_keyframe_request(&r));
    sc_recording_request_stop(&r);
    wait_gate(&gate.close_entered);
    assert_responsive(&r, false);
    assert(!sc_recording_request_start(&r, directory, 1794100001000));
    release_gate(&gate.release_close);
    sc_recording_stop(&r);
    assert_empty(directory);

    // The keyframe deadline starts only after file/sink setup, and timeout
    // merely requests a stop; it never joins or touches the filesystem.
    reset_gate(false, false);
    assert(sc_recording_start(&r, directory, 1794100001222));
    sc_mutex_lock(&r.mutex);
    r.requested_ms -= 11000;
    sc_mutex_unlock(&r.mutex);
    bool ready, active;
    char message[1024];
    sc_recording_poll(&r, &ready, &active, message, sizeof(message));
    sc_recording_stop(&r);
    assert(strstr(r.message, "No keyframe received"));
    assert_empty(directory);

    sc_recording_destroy(&r);
    assert(!rmdir(directory));
    sc_cond_destroy(&gate.cond);
    sc_mutex_destroy(&gate.mutex);
    SDL_Quit();
    return 0;
}
