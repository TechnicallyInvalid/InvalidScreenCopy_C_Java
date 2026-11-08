#include "recording.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavutil/time.h>

#include "util/log.h"

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
    if (r->active && r->accepting) {
        r->accepting = false;
        stop_writer(r);
    }
    sc_mutex_unlock(&r->mutex);
}

static void
recording_input_disable(struct sc_packet_sink *sink) {
    struct sc_recording_input *input = input_from_sink(sink);
    struct sc_recording *r = input->owner;
    sc_mutex_lock(&r->mutex);
    r->audio_expected = false;
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
                r->accepting = false;
                stop_writer(r);
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
        if (r->active && r->accepting) {
            r->accepting = false;
            stop_writer(r);
        }
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
    r->video.params = avcodec_parameters_alloc();
    r->audio.params = avcodec_parameters_alloc();
    if (!r->video.params || !r->audio.params) {
        avcodec_parameters_free(&r->video.params);
        avcodec_parameters_free(&r->audio.params);
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
    return true;
}

static bool
open_recorder_input(struct sc_recording_input *input, struct sc_packet_sink *sink) {
    if (!sink->ops->open(sink, input->params, NULL)) {
        return false;
    }
    return !input->config || sink->ops->push(sink, input->config) == SC_SINK_OK;
}

bool
sc_recording_start(struct sc_recording *r, const char *directory, int64_t unix_ms) {
    sc_mutex_lock(&r->mutex);
    if (r->active || !input_ready(&r->video)
            || (r->audio_expected && !input_ready(&r->audio))) {
        snprintf(r->message, sizeof(r->message), "Video/audio stream is not ready.");
        sc_mutex_unlock(&r->mutex);
        return false;
    }
    char *path;
    FILE *file = sc_recording_create_file(directory, unix_ms, &path);
    if (!file) {
        snprintf(r->message, sizeof(r->message), "Cannot create recording: %s", strerror(errno));
        sc_mutex_unlock(&r->mutex);
        return false;
    }
    struct sc_recorder *rec = calloc(1, sizeof(*rec));
    static const struct sc_recorder_callbacks callbacks = {.on_ended = recording_ended};
    if (!rec || !sc_recorder_init(rec, path, SC_RECORD_FORMAT_MP4, true,
            r->audio_expected, r->orientation, &callbacks, r)) {
        free(rec);
        fclose(file);
        unlink(path);
        free(path);
        snprintf(r->message, sizeof(r->message), "Cannot allocate recording.");
        sc_mutex_unlock(&r->mutex);
        return false;
    }
    rec->output_file = file;
    if (!sc_recorder_start(rec)) {
        sc_recorder_destroy(rec);
        free(rec);
        unlink(path);
        free(path);
        snprintf(r->message, sizeof(r->message), "Cannot open MP4 writer.");
        sc_mutex_unlock(&r->mutex);
        return false;
    }
    r->active = rec;
    r->completed = false;
    r->success = false;
    r->has_frame = false;
    r->requested_ms = av_gettime_relative() / 1000;
    bool ok = open_recorder_input(&r->video, &rec->video_packet_sink);
    if (ok && r->audio_expected) {
        ok = open_recorder_input(&r->audio, &rec->audio_packet_sink);
    }
    r->accepting = ok;
    if (!ok) {
        sc_recorder_stop(rec);
    }
    snprintf(r->message, sizeof(r->message), "Waiting for keyframe: %s", path);
    free(path);
    sc_mutex_unlock(&r->mutex);
    return ok;
}

void
sc_recording_stop(struct sc_recording *r) {
    sc_mutex_lock(&r->mutex);
    struct sc_recorder *rec = r->active;
    if (!rec) {
        sc_mutex_unlock(&r->mutex);
        return;
    }
    r->accepting = false;
    stop_writer(r);
    sc_mutex_unlock(&r->mutex);
    sc_recorder_join(rec);
    sc_mutex_lock(&r->mutex);
    if (!r->has_frame) {
        // No decodable frame was captured; don't leave a misleading empty clip.
        unlink(rec->filename);
        snprintf(r->message, sizeof(r->message), "No video frame captured. Wake the phone and try again.");
    } else {
        snprintf(r->message, sizeof(r->message), "%s: %s",
            r->success ? "Saved" : "Recording failed; partial file kept", rec->filename);
    }
    sc_recorder_destroy(rec);
    free(rec);
    r->active = NULL;
    sc_mutex_unlock(&r->mutex);
}

void
sc_recording_poll(struct sc_recording *r, bool *ready, bool *active,
                  char *message, size_t size) {
    sc_mutex_lock(&r->mutex);
    bool reap = r->active && (r->completed || (!r->has_frame
        && av_gettime_relative() / 1000 - r->requested_ms > 10000));
    sc_mutex_unlock(&r->mutex);
    if (reap) {
        sc_recording_stop(r);
    }
    sc_mutex_lock(&r->mutex);
    *ready = input_ready(&r->video)
        && (!r->audio_expected || input_ready(&r->audio));
    *active = r->active != NULL;
    if (r->active && r->has_frame) {
        snprintf(message, size, "Recording: %s", r->active->filename);
    } else {
        snprintf(message, size, "%s", r->message);
    }
    sc_mutex_unlock(&r->mutex);
}

void
sc_recording_destroy(struct sc_recording *r) {
    sc_recording_stop(r);
    avcodec_parameters_free(&r->video.params);
    avcodec_parameters_free(&r->audio.params);
    av_packet_free(&r->video.config);
    av_packet_free(&r->audio.config);
    sc_mutex_destroy(&r->mutex);
}
