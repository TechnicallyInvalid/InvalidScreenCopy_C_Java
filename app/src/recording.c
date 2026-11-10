#include "recording.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavutil/time.h>

#include "util/log.h"

struct sc_recording_request {
    char *directory;
    int64_t unix_ms;
    uint64_t generation;
    struct sc_recording_input video;
    struct sc_recording_input audio;
    bool audio_expected;
};

static int recording_worker(void *userdata);
static void request_stop_locked(struct sc_recording *r);

static struct sc_recording_input *
input_from_sink(struct sc_packet_sink *sink) {
    return container_of(sink, struct sc_recording_input, sink);
}

static void
recording_ended(struct sc_recorder *recorder, bool success, void *userdata) {
    (void) recorder;
    struct sc_recording *r = userdata;
    sc_mutex_lock(&r->mutex);
    r->success = success;
    r->completed = true;
    r->accepting = false;
    sc_cond_broadcast(&r->cond);
    sc_mutex_unlock(&r->mutex);
}

static bool
recording_input_open(struct sc_packet_sink *sink, const AVCodecParameters *params,
                     const struct sc_stream_session *session) {
    (void) session;
    struct sc_recording_input *input = input_from_sink(sink);
    struct sc_recording *r = input->owner;
    sc_mutex_lock(&r->mutex);
    bool ok = avcodec_parameters_copy(input->params, params) >= 0;
    input->ready = ok;
    ++r->generation;
    sc_mutex_unlock(&r->mutex);
    return ok;
}

static void
stop_writer(struct sc_recording *r) {
    sc_recorder_stop_with_duration(r->active,
        r->has_frame ? av_gettime_relative() - r->first_frame_time : 0);
}

static void
recording_input_close(struct sc_packet_sink *sink) {
    struct sc_recording_input *input = input_from_sink(sink);
    struct sc_recording *r = input->owner;
    sc_mutex_lock(&r->mutex);
    input->ready = false;
    ++r->generation;
    if (r->busy) request_stop_locked(r);
    sc_mutex_unlock(&r->mutex);
}

static void
recording_input_disable(struct sc_packet_sink *sink) {
    struct sc_recording_input *input = input_from_sink(sink);
    struct sc_recording *r = input->owner;
    sc_mutex_lock(&r->mutex);
    r->audio_expected = false;
    ++r->generation;
    sc_mutex_unlock(&r->mutex);
}

static enum sc_sink_result
recording_input_push(struct sc_packet_sink *sink, const AVPacket *packet) {
    struct sc_recording_input *input = input_from_sink(sink);
    struct sc_recording *r = input->owner;
    sc_mutex_lock(&r->mutex);
    if (packet->pts == AV_NOPTS_VALUE) {
        av_packet_free(&input->config);
        input->config = av_packet_clone(packet);
    }
    if (r->active && r->accepting) {
        bool forward = packet->pts == AV_NOPTS_VALUE;
        if (!forward && input->video) {
            if (!r->has_frame && (packet->flags & AV_PKT_FLAG_KEY)) {
                r->has_frame = true;
                r->first_pts = packet->pts;
                r->first_frame_time = av_gettime_relative();
            }
            forward = r->has_frame;
        } else if (!forward) {
            // Drop pre-boundary audio so every clip starts at its own keyframe.
            forward = r->has_frame && packet->pts >= r->first_pts;
        }
        if (forward) {
            struct sc_packet_sink *target = input->video
                ? &r->active->video_packet_sink : &r->active->audio_packet_sink;
            if (target->ops->push(target, packet) != SC_SINK_OK) {
                request_stop_locked(r);
            }
        }
    }
    sc_mutex_unlock(&r->mutex);
    // A recording failure must never terminate mirroring.
    return SC_SINK_OK;
}

static enum sc_sink_result
recording_input_session(struct sc_packet_sink *sink,
                        const struct sc_stream_session *session) {
    struct sc_recording_input *input = input_from_sink(sink);
    struct sc_recording *r = input->owner;
    sc_mutex_lock(&r->mutex);
    if (input->params->width != (int) session->video.width
            || input->params->height != (int) session->video.height) {
        // MP4 cannot reliably change dimensions within a stream. Finalize the
        // old clip, and use fresh metadata/config for the next button press.
        if (r->busy) request_stop_locked(r);
        ++r->generation;
        input->params->width = session->video.width;
        input->params->height = session->video.height;
        av_packet_free(&input->config);
        snprintf(r->message, sizeof(r->message),
                 "Display size changed. Start a new clip when ready.");
    }
    sc_mutex_unlock(&r->mutex);
    return SC_SINK_OK;
}

static bool
input_ready(const struct sc_recording_input *input) {
    enum AVCodecID id = input->params->codec_id;
    return input->ready && (input->config || id == AV_CODEC_ID_VP8
        || id == AV_CODEC_ID_VP9 || id == AV_CODEC_ID_PCM_S16LE);
}

bool
sc_recording_init(struct sc_recording *r, bool audio,
                   enum sc_orientation orientation) {
    memset(r, 0, sizeof(*r));
    if (!sc_mutex_init(&r->mutex)) {
        return false;
    }
    if (!sc_cond_init(&r->cond)) {
        sc_mutex_destroy(&r->mutex);
        return false;
    }
    r->video.params = avcodec_parameters_alloc();
    r->audio.params = avcodec_parameters_alloc();
    if (!r->video.params || !r->audio.params) {
        avcodec_parameters_free(&r->video.params);
        avcodec_parameters_free(&r->audio.params);
        sc_cond_destroy(&r->cond);
        sc_mutex_destroy(&r->mutex);
        return false;
    }
    static const struct sc_packet_sink_ops video_ops = {
        .open = recording_input_open, .close = recording_input_close,
        .push = recording_input_push, .push_session = recording_input_session,
    };
    static const struct sc_packet_sink_ops audio_ops = {
        .open = recording_input_open, .close = recording_input_close,
        .push = recording_input_push, .disable = recording_input_disable,
    };
    r->video.sink.ops = &video_ops;
    r->audio.sink.ops = &audio_ops;
    r->video.owner = r;
    r->audio.owner = r;
    r->video.video = true;
    r->audio_expected = audio;
    r->orientation = orientation;
    snprintf(r->message, sizeof(r->message), "Waiting for the video stream…");
    if (!sc_thread_create(&r->worker, recording_worker, "record-manager", r)) {
        avcodec_parameters_free(&r->video.params);
        avcodec_parameters_free(&r->audio.params);
        sc_cond_destroy(&r->cond);
        sc_mutex_destroy(&r->mutex);
        return false;
    }
    return true;
}

static bool
open_recorder_input(struct sc_recording_input *input, struct sc_packet_sink *sink) {
    if (!sink->ops->open(sink, input->params, NULL)) {
        return false;
    }
    return !input->config || sink->ops->push(sink, input->config) == SC_SINK_OK;
}

static void
free_request(struct sc_recording_request *request) {
    if (!request) return;
    free(request->directory);
    avcodec_parameters_free(&request->video.params);
    avcodec_parameters_free(&request->audio.params);
    av_packet_free(&request->video.config);
    av_packet_free(&request->audio.config);
    free(request);
}

static bool
copy_input(struct sc_recording_input *dst,
           const struct sc_recording_input *src) {
    dst->params = avcodec_parameters_alloc();
    if (!dst->params || avcodec_parameters_copy(dst->params, src->params) < 0) {
        return false;
    }
    if (src->config) {
        dst->config = av_packet_clone(src->config);
        if (!dst->config) return false;
    }
    return true;
}

bool
sc_recording_request_start(struct sc_recording *r, const char *directory,
                            int64_t unix_ms) {
    struct sc_recording_request *request = calloc(1, sizeof(*request));
    if (!request) return false;
    request->directory = strdup(directory);
    if (!request->directory) {
        free_request(request);
        return false;
    }
    request->unix_ms = unix_ms;
    sc_mutex_lock(&r->mutex);
    if (r->busy || r->worker_stopped || !input_ready(&r->video)
            || (r->audio_expected && !input_ready(&r->audio))) {
        if (!r->busy) {
            snprintf(r->message, sizeof(r->message), "Video/audio stream is not ready.");
        }
        sc_mutex_unlock(&r->mutex);
        free_request(request);
        return false;
    }
    request->audio_expected = r->audio_expected;
    request->generation = r->generation;
    bool ok = copy_input(&request->video, &r->video)
        && (!request->audio_expected || copy_input(&request->audio, &r->audio));
    if (!ok) {
        snprintf(r->message, sizeof(r->message), "Cannot allocate recording metadata.");
        sc_mutex_unlock(&r->mutex);
        free_request(request);
        return false;
    }
    r->request = request;
    r->busy = true;
    r->starting = true;
    r->stop_requested = false;
    r->completed = false;
    r->success = false;
    r->has_frame = false;
    r->keyframe_requested = false;
    snprintf(r->message, sizeof(r->message), "Opening recording file…");
    sc_cond_broadcast(&r->cond);
    sc_mutex_unlock(&r->mutex);
    return true;
}

// Called only by the lifecycle worker. File I/O, recorder joins and destruction
// never happen on the UI thread or while holding the stream input mutex.
static void
run_recording(struct sc_recording *r, struct sc_recording_request *request) {
    char *path = NULL;
    char message[sizeof(r->message)];
    struct sc_recorder *rec = NULL;
    sc_mutex_lock(&r->mutex);
    bool cancelled = r->stop_requested;
    sc_mutex_unlock(&r->mutex);
    if (cancelled) {
        snprintf(message, sizeof(message), "Recording cancelled.");
        goto done;
    }
    FILE *file = sc_recording_create_file(request->directory, request->unix_ms, &path);
    if (!file) {
        snprintf(message, sizeof(message), "Cannot create recording: %s", strerror(errno));
        goto done;
    }
    rec = calloc(1, sizeof(*rec));
    static const struct sc_recorder_callbacks callbacks = {.on_ended = recording_ended};
    if (!rec || !sc_recorder_init(rec, path, SC_RECORD_FORMAT_MP4, true,
            request->audio_expected, r->orientation, &callbacks, r)) {
        free(rec);
        rec = NULL;
        fclose(file);
        unlink(path);
        snprintf(message, sizeof(message), "Cannot allocate recording.");
        goto done;
    }
    rec->output_file = file;
    if (!sc_recorder_start(rec)) {
        sc_recorder_destroy(rec);
        free(rec);
        rec = NULL;
        unlink(path);
        snprintf(message, sizeof(message), "Cannot open MP4 writer.");
        goto done;
    }
    bool ok = open_recorder_input(&request->video, &rec->video_packet_sink);
    if (ok && request->audio_expected) {
        ok = open_recorder_input(&request->audio, &rec->audio_packet_sink);
    }
    sc_mutex_lock(&r->mutex);
    r->active = rec;
    r->starting = false;
    bool changed = request->generation != r->generation;
    r->accepting = ok && !r->stop_requested && !r->completed && !changed;
    if (r->accepting) {
        r->requested_ms = av_gettime_relative() / 1000;
        r->keyframe_requested = true;
        snprintf(r->message, sizeof(r->message), "Waiting for keyframe: %s", path);
    } else {
        r->stop_requested = true;
        stop_writer(r);
    }
    sc_cond_broadcast(&r->cond);
    while (!r->completed && !r->stop_requested) {
        sc_cond_wait(&r->cond, &r->mutex);
    }
    r->accepting = false;
    r->keyframe_requested = false;
    stop_writer(r);
    snprintf(r->message, sizeof(r->message), "Finishing recording…");
    sc_mutex_unlock(&r->mutex);

    sc_recorder_join(rec);
    // With accepting=false, demuxers cannot access the writer. Detach it before
    // closing/removing files so even slow filesystem cleanup cannot stall them.
    sc_mutex_lock(&r->mutex);
    r->active = NULL;
    bool has_frame = r->has_frame;
    bool success = r->success;
    sc_mutex_unlock(&r->mutex);
    if (!has_frame) {
        unlink(rec->filename);
        snprintf(message, sizeof(message), "%s", changed
            ? "Stream changed before recording started. Try again."
            : "No keyframe received. Change the phone screen and try recording again.");
    } else {
        snprintf(message, sizeof(message), "%s: %s",
            success ? "Saved" : "Recording failed; partial file kept", rec->filename);
    }
    sc_recorder_destroy(rec);
    free(rec);

done:
    free(path);
    free_request(request);
    sc_mutex_lock(&r->mutex);
    r->starting = false;
    r->busy = false;
    r->accepting = false;
    r->keyframe_requested = false;
    snprintf(r->message, sizeof(r->message), "%s", message);
    sc_cond_broadcast(&r->cond);
    sc_mutex_unlock(&r->mutex);
}

static int
recording_worker(void *userdata) {
    struct sc_recording *r = userdata;
    sc_mutex_lock(&r->mutex);
    for (;;) {
        while (!r->request && !r->worker_stopped) {
            sc_cond_wait(&r->cond, &r->mutex);
        }
        if (r->worker_stopped && !r->request) break;
        struct sc_recording_request *request = r->request;
        r->request = NULL;
        sc_mutex_unlock(&r->mutex);
        run_recording(r, request);
        sc_mutex_lock(&r->mutex);
    }
    sc_mutex_unlock(&r->mutex);
    return 0;
}

bool
sc_recording_start(struct sc_recording *r, const char *directory, int64_t unix_ms) {
    if (!sc_recording_request_start(r, directory, unix_ms)) return false;
    sc_mutex_lock(&r->mutex);
    while (r->starting) sc_cond_wait(&r->cond, &r->mutex);
    bool ok = r->accepting;
    sc_mutex_unlock(&r->mutex);
    return ok;
}

static void
request_stop_locked(struct sc_recording *r) {
    if (!r->busy) return;
    r->stop_requested = true;
    r->accepting = false;
    r->keyframe_requested = false;
    if (r->active) stop_writer(r);
    snprintf(r->message, sizeof(r->message), "Finishing recording…");
    sc_cond_broadcast(&r->cond);
}

void
sc_recording_request_stop(struct sc_recording *r) {
    sc_mutex_lock(&r->mutex);
    request_stop_locked(r);
    sc_mutex_unlock(&r->mutex);
}

bool
sc_recording_can_stop(struct sc_recording *r) {
    sc_mutex_lock(&r->mutex);
    bool can_stop = r->busy && !r->stop_requested && !r->completed;
    sc_mutex_unlock(&r->mutex);
    return can_stop;
}

void
sc_recording_stop(struct sc_recording *r) {
    sc_mutex_lock(&r->mutex);
    request_stop_locked(r);
    while (r->busy) sc_cond_wait(&r->cond, &r->mutex);
    sc_mutex_unlock(&r->mutex);
}

bool
sc_recording_take_keyframe_request(struct sc_recording *r) {
    sc_mutex_lock(&r->mutex);
    bool requested = r->keyframe_requested && r->accepting;
    r->keyframe_requested = false;
    sc_mutex_unlock(&r->mutex);
    return requested;
}

void
sc_recording_poll(struct sc_recording *r, bool *ready, bool *active,
                  char *message, size_t size) {
    sc_mutex_lock(&r->mutex);
    if (r->active && r->accepting && !r->has_frame
            && av_gettime_relative() / 1000 - r->requested_ms > 10000) {
        request_stop_locked(r);
    }
    *ready = input_ready(&r->video)
        && (!r->audio_expected || input_ready(&r->audio));
    *active = r->busy;
    if (r->active && r->has_frame && r->accepting) {
        snprintf(message, size, "Recording: %s", r->active->filename);
    } else {
        snprintf(message, size, "%s", r->message);
    }
    sc_mutex_unlock(&r->mutex);
}

void
sc_recording_destroy(struct sc_recording *r) {
    sc_mutex_lock(&r->mutex);
    request_stop_locked(r);
    r->worker_stopped = true;
    sc_cond_broadcast(&r->cond);
    sc_mutex_unlock(&r->mutex);
    sc_thread_join(&r->worker, NULL);
    avcodec_parameters_free(&r->video.params);
    avcodec_parameters_free(&r->audio.params);
    av_packet_free(&r->video.config);
    av_packet_free(&r->audio.config);
    sc_cond_destroy(&r->cond);
    sc_mutex_destroy(&r->mutex);
}
