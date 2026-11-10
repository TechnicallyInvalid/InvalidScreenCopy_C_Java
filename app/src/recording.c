#include "recording.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavutil/time.h>

#include "recording_audio.h"
#include "util/log.h"

struct sc_recording_request {
    char *directory;
    int64_t unix_ms;
    uint64_t generation;
    struct sc_recording_input video;
    struct sc_recording_input audio;
    bool audio_expected;
    unsigned microphones;
    uint32_t microphone_session;
};

static int recording_worker(void *userdata);
static void request_stop_locked(struct sc_recording *r);

static void
microphone_fail_locked(struct sc_recording *r, const char *message) {
    if (!r->microphone_error[0]) {
        snprintf(r->microphone_error, sizeof(r->microphone_error), "%s", message);
    }
    request_stop_locked(r);
}

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
    if (r->has_frame && r->stop_pts == INT64_MAX) {
        r->stop_pts = r->first_pts + av_gettime_relative() - r->first_frame_time;
    }
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
    if (!input->video && ok) r->audio_expected = true;
    ++r->generation;
    sc_mutex_unlock(&r->mutex);
    return ok;
}

static void
stop_writer(struct sc_recording *r) {
    sc_recorder_stop_with_duration(r->active,
        r->has_frame ? (r->stop_pts != INT64_MAX
            ? r->stop_pts - r->first_pts
            : av_gettime_relative() - r->first_frame_time) : 0);
}

static void
recording_input_close(struct sc_packet_sink *sink) {
    struct sc_recording_input *input = input_from_sink(sink);
    struct sc_recording *r = input->owner;
    sc_mutex_lock(&r->mutex);
    input->ready = false;
    ++r->generation;
    if (r->busy) {
        if (!input->video && r->device_output && !r->stop_requested)
            microphone_fail_locked(r, "Android device audio ended unexpectedly; recording stopped.");
        else if (r->mixing && !r->stop_requested) microphone_fail_locked(r, "Device stream disconnected during microphone recording.");
        else request_stop_locked(r);
    } else if (!input->video && r->device_output) {
        snprintf(r->message, sizeof(r->message), "Android device audio is unavailable. Reconnect the display before recording.");
    }
    sc_mutex_unlock(&r->mutex);
}

static void
recording_input_disable(struct sc_packet_sink *sink) {
    struct sc_recording_input *input = input_from_sink(sink);
    struct sc_recording *r = input->owner;
    sc_mutex_lock(&r->mutex);
    r->audio_expected = false;
    input->ready = false;
    ++r->generation;
    if (r->device_output && (!r->busy || !r->stop_requested)) {
        if (r->busy) microphone_fail_locked(r, "Android device audio is unavailable; recording stopped.");
        else snprintf(r->message, sizeof(r->message), "Android device audio is unavailable. Reconnect the display before recording.");
    }
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
    if (r->active && r->accepting && (input->video || !r->mixing)) {
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

static bool
mixed_audio_open(struct sc_packet_sink *sink, const AVCodecParameters *params,
                  const struct sc_stream_session *session) {
    struct sc_recording *r = container_of(sink, struct sc_recording, mixed_audio_sink);
    sc_mutex_lock(&r->mutex);
    struct sc_packet_sink *target = &r->active->audio_packet_sink;
    bool ok = target->ops->open(target, params, session);
    sc_mutex_unlock(&r->mutex);
    return ok;
}

static void
mixed_audio_close(struct sc_packet_sink *sink) {
    (void) sink; // The manager stops the recorder only after encoder flushing.
}

static enum sc_sink_result
mixed_audio_push(struct sc_packet_sink *sink, const AVPacket *packet) {
    struct sc_recording *r = container_of(sink, struct sc_recording, mixed_audio_sink);
    sc_mutex_lock(&r->mutex);
    bool config = packet->pts == AV_NOPTS_VALUE;
    bool forward = r->active && (config || ((r->accepting || r->mixed_draining)
        && r->has_frame && packet->pts >= r->first_pts && packet->pts < r->stop_pts));
    enum sc_sink_result result = SC_SINK_OK;
    if (forward) {
        struct sc_packet_sink *target = &r->active->audio_packet_sink;
        result = target->ops->push(target, packet);
        if (result != SC_SINK_OK) {
            microphone_fail_locked(r, "Mixed audio could not be written; partial recording kept.");
        }
    }
    sc_mutex_unlock(&r->mutex);
    return result;
}

static bool
recording_audio_frames_open(struct sc_frame_sink *sink, const AVCodecContext *ctx,
                            const struct sc_stream_session *session) {
    (void) ctx; (void) session;
    struct sc_recording *r = container_of(sink, struct sc_recording, audio_frame_sink);
    sc_mutex_lock(&r->mutex);
    r->audio_frames_ready = true;
    sc_mutex_unlock(&r->mutex);
    return true;
}

static void
recording_audio_frames_close(struct sc_frame_sink *sink) {
    struct sc_recording *r = container_of(sink, struct sc_recording, audio_frame_sink);
    sc_mutex_lock(&r->mutex);
    r->audio_frames_ready = false;
    if (r->busy && r->mixing && !r->stop_requested) {
        microphone_fail_locked(r, "Android device audio ended during microphone recording.");
    }
    sc_mutex_unlock(&r->mutex);
}

static enum sc_sink_result
recording_audio_frames_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_recording *r = container_of(sink, struct sc_recording, audio_frame_sink);
    sc_mutex_lock(&r->mutex);
    if (r->mixer && !r->stop_requested
            && r->seen_microphones == r->capture_microphones) {
        if (!sc_recording_audio_push(r->mixer, SC_RECORDING_AUDIO_DEVICE, frame, frame->pts)) {
            char error[512] = "";
            sc_recording_audio_failed(r->mixer, error, sizeof(error));
            microphone_fail_locked(r, error[0] ? error : "Device audio could not be queued for microphone recording.");
        }
    }
    sc_mutex_unlock(&r->mutex);
    return SC_SINK_OK; // Recorder failures must not terminate live playback.
}

void
sc_recording_set_microphone_backend(struct sc_recording *r,
    const struct sc_recording_microphone_ops *ops, void *userdata, bool device_output) {
    sc_mutex_lock(&r->mutex);
    assert(!r->busy);
    r->microphone_ops = ops;
    r->microphone_userdata = userdata;
    r->device_output = device_output;
    sc_mutex_unlock(&r->mutex);
}

static bool
microphones_available_locked(struct sc_recording *r) {
    return r->microphone_ops && r->device_output && r->audio_expected;
}

bool
sc_recording_microphones_available(struct sc_recording *r) {
    sc_mutex_lock(&r->mutex);
    bool available = microphones_available_locked(r);
    sc_mutex_unlock(&r->mutex);
    return available;
}

bool
sc_recording_set_microphones(struct sc_recording *r, bool android_mic, bool local_mic) {
    sc_mutex_lock(&r->mutex);
    bool ok = !r->busy && (!(android_mic || local_mic) || microphones_available_locked(r));
    if (ok) {
        r->selected_microphones = (android_mic ? SC_RECORDING_ANDROID_MIC : 0)
                                | (local_mic ? SC_RECORDING_LOCAL_MIC : 0);
    }
    sc_mutex_unlock(&r->mutex);
    return ok;
}

void
sc_recording_get_microphones(struct sc_recording *r, bool *android_mic, bool *local_mic) {
    sc_mutex_lock(&r->mutex);
    *android_mic = (r->selected_microphones & SC_RECORDING_ANDROID_MIC) != 0;
    *local_mic = (r->selected_microphones & SC_RECORDING_LOCAL_MIC) != 0;
    sc_mutex_unlock(&r->mutex);
}

void
sc_recording_set_status(struct sc_recording *r, const char *message) {
    sc_mutex_lock(&r->mutex);
    if (!r->busy) snprintf(r->message, sizeof(r->message), "%s", message);
    sc_mutex_unlock(&r->mutex);
}

void
sc_recording_microphone_error(struct sc_recording *r, uint32_t session, const char *message) {
    sc_mutex_lock(&r->mutex);
    if (r->busy && r->mixing && session == r->microphone_session && !r->stop_requested) {
        microphone_fail_locked(r, message);
    }
    sc_mutex_unlock(&r->mutex);
}

void
sc_recording_android_status(struct sc_recording *r, uint32_t session,
                            uint8_t status, const char *message) {
    sc_mutex_lock(&r->mutex);
    if (r->busy && r->mixing && session == r->microphone_session
            && (r->capture_microphones & SC_RECORDING_ANDROID_MIC)) {
        if (status == 2) {
            // An explicit source failure is not a normal Stop acknowledgement,
            // even if it arrives while the manager is draining a requested stop.
            microphone_fail_locked(r, message && *message ? message : "Android microphone capture failed.");
        } else if (!r->stop_requested) {
            if (status == 1) r->android_ready = true;
            else microphone_fail_locked(r, message && *message ? message : "Android microphone stopped unexpectedly.");
        }
        sc_cond_broadcast(&r->cond);
    }
    sc_mutex_unlock(&r->mutex);
}

static void
recording_microphone_pcm(struct sc_recording *r, uint32_t session, int64_t pts_us,
                         const void *pcm, size_t frames, enum AVSampleFormat format,
                         enum sc_recording_audio_source source) {
    sc_mutex_lock(&r->mutex);
    unsigned bit = 1u << source;
    if (!r->mixer || r->stop_requested || session != r->microphone_session
            || !(r->capture_microphones & bit)) {
        sc_mutex_unlock(&r->mutex);
        return;
    }
    if (!pcm || !frames || frames > 4800 || pts_us < 0) {
        microphone_fail_locked(r, "Microphone supplied invalid audio samples or timestamps.");
        sc_mutex_unlock(&r->mutex);
        return;
    }
    r->seen_microphones |= bit;
    // These samples precede video acceptance. Do not accumulate one microphone
    // while another selected source is still opening/calibrating.
    if (r->seen_microphones != r->capture_microphones) {
        sc_cond_broadcast(&r->cond);
        sc_mutex_unlock(&r->mutex);
        return;
    }
    AVFrame *frame = av_frame_alloc();
    bool ok = frame != NULL;
    if (frame) {
        frame->format = format;
        frame->sample_rate = 48000;
        frame->nb_samples = (int) frames;
        frame->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_MONO;
        ok = av_frame_get_buffer(frame, 0) >= 0;
        if (ok) {
            memcpy(frame->data[0], pcm, frames * av_get_bytes_per_sample(format));
            ok = sc_recording_audio_push(r->mixer, source, frame, pts_us);
        }
        av_frame_free(&frame);
    }
    if (!ok) {
        char error[512] = "";
        sc_recording_audio_failed(r->mixer, error, sizeof(error));
        microphone_fail_locked(r, error[0] ? error : "Microphone audio could not be buffered for recording.");
    }
    sc_cond_broadcast(&r->cond);
    sc_mutex_unlock(&r->mutex);
}

void
sc_recording_android_pcm(struct sc_recording *r, uint32_t session, int64_t pts_us,
                         const uint8_t *pcm, uint16_t bytes) {
    if (bytes & 1) {
        sc_recording_microphone_error(r, session, "Android microphone PCM has an invalid byte count.");
        return;
    }
    recording_microphone_pcm(r, session, pts_us, pcm, bytes / 2,
                             AV_SAMPLE_FMT_S16, SC_RECORDING_AUDIO_ANDROID_MIC);
}

void
sc_recording_local_pcm(struct sc_recording *r, uint32_t session, int64_t server_pts_us,
                       const float *pcm, size_t frames) {
    recording_microphone_pcm(r, session, server_pts_us, pcm, frames,
                             AV_SAMPLE_FMT_FLT, SC_RECORDING_AUDIO_LOCAL_MIC);
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
    static const struct sc_frame_sink_ops frame_ops = {
        .open = recording_audio_frames_open, .close = recording_audio_frames_close,
        .push = recording_audio_frames_push,
    };
    static const struct sc_packet_sink_ops mixed_ops = {
        .open = mixed_audio_open, .close = mixed_audio_close, .push = mixed_audio_push,
    };
    r->audio_frame_sink.ops = &frame_ops;
    r->mixed_audio_sink.ops = &mixed_ops;
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
    if (!r->busy && r->device_output && !input_ready(&r->audio)) {
        snprintf(r->message, sizeof(r->message), "Android device audio is unavailable. Reconnect the display before recording.");
        sc_mutex_unlock(&r->mutex);
        free_request(request);
        return false;
    }
    if (r->busy || r->worker_stopped || !input_ready(&r->video)
            || (r->audio_expected && !input_ready(&r->audio))) {
        if (!r->busy) {
            snprintf(r->message, sizeof(r->message), "Video/audio stream is not ready.");
        }
        sc_mutex_unlock(&r->mutex);
        free_request(request);
        return false;
    }
    if (r->selected_microphones && (!microphones_available_locked(r) || !r->audio_frames_ready)) {
        snprintf(r->message, sizeof(r->message), "Microphones require ready Android device-output audio and device control.");
        sc_mutex_unlock(&r->mutex);
        free_request(request);
        return false;
    }
    request->audio_expected = r->audio_expected;
    request->generation = r->generation;
    request->microphones = r->selected_microphones;
    if (++r->microphone_session == 0) ++r->microphone_session;
    request->microphone_session = r->microphone_session;
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
    r->microphone_error[0] = '\0';
    r->capture_microphones = request->microphones;
    r->seen_microphones = 0;
    r->android_ready = false;
    r->mixing = request->microphones != 0;
    r->mixed_draining = false;
    r->stop_pts = INT64_MAX;
    snprintf(r->message, sizeof(r->message), "Opening recording file…");
    sc_cond_broadcast(&r->cond);
    sc_mutex_unlock(&r->mutex);
    return true;
}

static bool
microphones_start(struct sc_recording *r, struct sc_recording_request *request,
                   struct sc_recording_audio *mixer, bool *android_started,
                   bool *local_started, uint32_t *clock_sequence) {
    const struct sc_recording_microphone_ops *ops = r->microphone_ops;
    void *userdata = r->microphone_userdata;
    uint32_t session = request->microphone_session;
    unsigned mask = request->microphones;
    sc_mutex_lock(&r->mutex);
    bool cancelled = r->stop_requested;
    sc_mutex_unlock(&r->mutex);
    if (cancelled) return false;
    if ((mask & SC_RECORDING_ANDROID_MIC) && !(mask & SC_RECORDING_LOCAL_MIC)) {
        *android_started = true; // A failed/late enable still requires a stop.
        if (!ops->set_android(userdata, true, session)) {
            sc_recording_microphone_error(r, session, "Could not request Android microphone capture.");
            return false;
        }
    }
    int64_t deadline = av_gettime_relative() + 3000000;
    int64_t next_query = 0;
    for (;;) {
        sc_mutex_lock(&r->mutex);
        bool cancelled = r->stop_requested || r->completed
            || request->generation != r->generation;
        bool android_ready = !(mask & SC_RECORDING_ANDROID_MIC) || r->android_ready;
        sc_mutex_unlock(&r->mutex);
        if (cancelled) return false;
        char error[512] = "";
        if (sc_recording_audio_failed(mixer, error, sizeof(error))) {
            sc_recording_microphone_error(r, session, error);
            return false;
        }
        int64_t now = av_gettime_relative();
        if (!(mask & SC_RECORDING_LOCAL_MIC) && now >= next_query) {
            // Android capture has a short renewable lease, so a stalled or
            // disconnected host cannot leave a microphone live indefinitely.
            if (!ops->query_clock(userdata, session, ++*clock_sequence)) {
                sc_recording_microphone_error(r, session, "Could not maintain the Android microphone capture session.");
                return false;
            }
            next_query = now + 2000000;
        }
        if (mask & SC_RECORDING_LOCAL_MIC) {
            if (*clock_sequence < 5 && now >= next_query) {
                if (!ops->query_clock(userdata, session, ++*clock_sequence)) {
                    sc_recording_microphone_error(r, session, "Could not synchronize the local microphone clock with Android.");
                    return false;
                }
                next_query = now + 100000;
            }
            if (!*local_started && *clock_sequence >= 5) {
                bool ready = ops->clock_ready(userdata, session, error, sizeof(error));
                if (!ready && error[0]) {
                    sc_recording_microphone_error(r, session, error);
                    return false;
                }
                if (ready) {
                    *local_started = true;
                    if (!ops->start_local(userdata, session, error, sizeof(error))) {
                        sc_recording_microphone_error(r, session, error[0] ? error : "Local microphone could not be opened.");
                        return false;
                    }
                }
            }
            if (*local_started && !ops->poll_local(userdata, error, sizeof(error))) {
                sc_recording_microphone_error(r, session, error[0] ? error : "Local microphone capture failed.");
                return false;
            }
            if (*local_started && (mask & SC_RECORDING_ANDROID_MIC) && !*android_started) {
                *android_started = true;
                if (!ops->set_android(userdata, true, session)) {
                    sc_recording_microphone_error(r, session, "Could not request Android microphone capture.");
                    return false;
                }
            }
        }
        if (android_ready && sc_recording_audio_ready(mixer)) return true;
        if (now >= deadline) {
            sc_recording_microphone_error(r, session, "Selected microphone audio did not become ready within three seconds.");
            return false;
        }
        sc_mutex_lock(&r->mutex);
        if (!r->stop_requested) {
            sc_cond_timedwait(&r->cond, &r->mutex, sc_tick_now() + SC_TICK_FROM_MS(20));
        }
        sc_mutex_unlock(&r->mutex);
    }
}

// Called only by the lifecycle worker. File I/O, recorder joins and destruction
// never happen on the UI thread or while holding the stream input mutex.
static void
run_recording(struct sc_recording *r, struct sc_recording_request *request) {
    char *path = NULL;
    char message[sizeof(r->message)];
    struct sc_recorder *rec = NULL;
    struct sc_recording_audio *mixer = NULL;
    bool android_started = false;
    bool local_started = false;
    bool mixer_started = false;
    uint32_t clock_sequence = 0;
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
    if (ok && request->audio_expected && !request->microphones) {
        ok = open_recorder_input(&request->audio, &rec->audio_packet_sink);
    }
    sc_mutex_lock(&r->mutex);
    r->active = rec;
    bool changed = request->generation != r->generation;
    bool setup_cancelled = r->stop_requested || r->completed || changed;
    sc_mutex_unlock(&r->mutex);
    if (ok && !setup_cancelled && request->microphones) {
        char error[512] = "";
        mixer = sc_recording_audio_create(request->microphones, &r->mixed_audio_sink,
                                          error, sizeof(error));
        ok = mixer && sc_recording_audio_start(mixer);
        mixer_started = ok;
        if (ok) {
            sc_mutex_lock(&r->mutex);
            r->mixer = mixer;
            bool start_capture = !r->stop_requested && !r->completed
                && request->generation == r->generation;
            r->microphone_cleanup_pending = start_capture;
            if (start_capture) snprintf(r->message, sizeof(r->message), "Starting selected microphones…");
            sc_mutex_unlock(&r->mutex);
            ok = start_capture && microphones_start(r, request, mixer, &android_started,
                                                    &local_started, &clock_sequence);
        } else {
            sc_recording_microphone_error(r, request->microphone_session,
                error[0] ? error : "Could not start mixed-audio recording.");
        }
    }
    sc_mutex_lock(&r->mutex);
    r->starting = false;
    changed = request->generation != r->generation;
    r->accepting = ok && !r->stop_requested && !r->completed && !changed;
    if (r->accepting) {
        r->requested_ms = av_gettime_relative() / 1000;
        r->keyframe_requested = true;
        snprintf(r->message, sizeof(r->message), "Waiting for keyframe: %s", path);
    } else {
        r->stop_requested = true;
        if (!r->mixing) stop_writer(r);
    }
    sc_cond_broadcast(&r->cond);
    int64_t next_clock_query = av_gettime_relative() + 2000000;
    while (!r->completed && !r->stop_requested) {
        if (!mixer) {
            sc_cond_wait(&r->cond, &r->mutex);
            continue;
        }
        sc_mutex_unlock(&r->mutex);
        char error[512] = "";
        bool failed = sc_recording_audio_failed(mixer, error, sizeof(error));
        int64_t now = av_gettime_relative();
        if (!failed && now >= next_clock_query) {
            failed = !r->microphone_ops->query_clock(r->microphone_userdata,
                request->microphone_session, ++clock_sequence);
            if (failed) snprintf(error, sizeof(error), "Microphone clock/session query failed.");
            next_clock_query = now + 2000000;
        }
        if (!failed && local_started) {
            failed = !r->microphone_ops->poll_local(r->microphone_userdata, error, sizeof(error));
            if (!failed && !r->microphone_ops->clock_ready(r->microphone_userdata,
                    request->microphone_session, error, sizeof(error))) {
                failed = true;
                if (!error[0]) snprintf(error, sizeof(error), "Local microphone lost clock synchronization.");
            }
        }
        if (failed) sc_recording_microphone_error(r, request->microphone_session,
            error[0] ? error : "Selected microphone capture failed.");
        sc_mutex_lock(&r->mutex);
        if (!r->completed && !r->stop_requested) {
            sc_cond_timedwait(&r->cond, &r->mutex, sc_tick_now() + SC_TICK_FROM_MS(100));
        }
    }
    request_stop_locked(r); // Freeze the first stop boundary before capture drain.
    r->accepting = false;
    r->keyframe_requested = false;
    r->mixer = NULL; // Detach all producers before stopping capture or draining.
    r->mixed_draining = mixer_started && !r->microphone_error[0];
    snprintf(r->message, sizeof(r->message), "Finishing recording…");
    sc_mutex_unlock(&r->mutex);

    // Microphones are closed before any possibly slow writer join or disk I/O.
    if (local_started) r->microphone_ops->stop_local(r->microphone_userdata);
    if (android_started && !r->microphone_ops->set_android(r->microphone_userdata,
            false, request->microphone_session)) {
        sc_mutex_lock(&r->mutex);
        snprintf(r->microphone_error, sizeof(r->microphone_error),
            "Android Mic stop was not confirmed; control connection closed to end capture.");
        sc_mutex_unlock(&r->mutex);
    }
    sc_mutex_lock(&r->mutex);
    r->microphone_cleanup_pending = false;
    sc_cond_broadcast(&r->cond);
    sc_mutex_unlock(&r->mutex);
    if (mixer) {
        if (mixer_started) {
            sc_recording_audio_stop(mixer);
            sc_recording_audio_join(mixer);
        }
        char error[512];
        if (sc_recording_audio_failed(mixer, error, sizeof(error))) {
            sc_mutex_lock(&r->mutex);
            if (!r->microphone_error[0]) snprintf(r->microphone_error,
                sizeof(r->microphone_error), "%s", error);
            sc_mutex_unlock(&r->mutex);
        }
        sc_recording_audio_destroy(mixer);
    }
    sc_mutex_lock(&r->mutex);
    r->mixed_draining = false;
    stop_writer(r);
    sc_mutex_unlock(&r->mutex);

    sc_recorder_join(rec);
    // With accepting=false, demuxers cannot access the writer. Detach it before
    // closing/removing files so even slow filesystem cleanup cannot stall them.
    sc_mutex_lock(&r->mutex);
    r->active = NULL;
    bool has_frame = r->has_frame;
    bool success = r->success && !r->microphone_error[0];
    char capture_error[sizeof(r->microphone_error)];
    snprintf(capture_error, sizeof(capture_error), "%s", r->microphone_error);
    sc_mutex_unlock(&r->mutex);
    if (!has_frame) {
        unlink(rec->filename);
        snprintf(message, sizeof(message), "%s", capture_error[0] ? capture_error : changed
            ? "Stream changed before recording started. Try again."
            : "No keyframe received. Change the phone screen and try recording again.");
    } else {
        if (capture_error[0]) snprintf(message, sizeof(message), "%s Partial file kept: %s",
            capture_error, rec->filename);
        else snprintf(message, sizeof(message), "%s: %s",
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
    r->mixing = false;
    r->capture_microphones = 0;
    r->seen_microphones = 0;
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
    if (r->has_frame && r->stop_pts == INT64_MAX) {
        r->stop_pts = r->first_pts + av_gettime_relative() - r->first_frame_time;
    }
    if (r->active && !r->mixing) stop_writer(r);
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
sc_recording_wait_microphones_stopped(struct sc_recording *r, sc_tick deadline) {
    sc_mutex_lock(&r->mutex);
    while (r->microphone_cleanup_pending) {
        if (!sc_cond_timedwait(&r->cond, &r->mutex, deadline)) break;
    }
    bool stopped = !r->microphone_cleanup_pending;
    sc_mutex_unlock(&r->mutex);
    return stopped;
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
        && (!(r->audio_expected || r->device_output) || input_ready(&r->audio));
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
