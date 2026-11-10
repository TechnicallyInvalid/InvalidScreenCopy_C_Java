#include "recording_audio.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "util/thread.h"

#define RATE 48000
#define CHANNELS 2
#define TIMELINE_SAMPLES RATE
#define MAX_QUEUED_BYTES (4u * 1024u * 1024u)
#define MAX_QUEUED_FRAMES 128u
#define STARTUP_TIMEOUT SC_TICK_FROM_SEC(3)
#define STALL_TIMEOUT SC_TICK_FROM_MS(500)
#define CLOCK_DEADBAND 96 // two milliseconds
#define MAX_CLOCK_JUMP (RATE / 20) // fifty milliseconds; larger gaps fail
#define MAX_COMPENSATION (RATE / 1000) // at most 0.1% clock-rate correction

struct queued_audio {
    struct queued_audio *next;
    AVFrame *frame;
    int64_t pts;
    size_t bytes;
};

struct source_audio {
    struct queued_audio *head;
    struct queued_audio *tail;
    unsigned count;
    SwrContext *swr;
    enum AVSampleFormat format;
    AVChannelLayout layout;
    int rate;
    float *samples;
    int64_t *positions;
    bool seen;
    int64_t first;
    int64_t end;
    double clock_error;
};

struct sc_recording_audio {
    sc_mutex mutex;
    sc_cond cond;
    sc_thread thread;
    bool started;
    bool joined;
    bool stopping;
    bool ready;
    bool failed;
    char error[256];
    unsigned mask;
    unsigned count;
    size_t queued_bytes;
    struct source_audio sources[SC_RECORDING_AUDIO_SOURCE_COUNT];
    struct sc_packet_sink *sink;
    bool sink_open;
    AVCodecContext *encoder;
    AVFrame *mixed;
    AVPacket *packet;
    bool origin_set;
    int64_t origin;
    bool cursor_set;
    int64_t cursor;
    int64_t last_packet_pts;
    sc_tick started_at;
    sc_tick progressed_at;
};

static const char *const source_names[] = {"Android device audio", "Android Mic", "Local Mic"};
static const AVRational micros = {1, 1000000};
static const AVRational samples = {1, RATE};

static void
fail_locked(struct sc_recording_audio *a, const char *message) {
    if (!a->failed) {
        a->failed = true;
        snprintf(a->error, sizeof(a->error), "%s", message);
    }
    sc_cond_broadcast(&a->cond);
}

static bool
fail(struct sc_recording_audio *a, const char *message) {
    sc_mutex_lock(&a->mutex);
    fail_locked(a, message);
    sc_mutex_unlock(&a->mutex);
    return false;
}

static bool
source_fail(struct sc_recording_audio *a, unsigned source, const char *reason) {
    char message[256];
    snprintf(message, sizeof(message), "%s: %s", source_names[source], reason);
    return fail(a, message);
}

static size_t
slot(int64_t position) {
    int64_t index = position % TIMELINE_SAMPLES;
    return index < 0 ? (size_t) (index + TIMELINE_SAMPLES) : (size_t) index;
}

static bool
contains(const struct source_audio *source, int64_t position) {
    return source->positions[slot(position)] == position;
}

static bool
receive_encoded(struct sc_recording_audio *a) {
    for (;;) {
        int result = avcodec_receive_packet(a->encoder, a->packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
        if (result < 0) return fail(a, "AAC encoder failed to produce a packet.");
        av_packet_rescale_ts(a->packet, samples, micros);
        a->packet->pts += a->origin;
        a->packet->dts = a->packet->pts;
        if (a->last_packet_pts != AV_NOPTS_VALUE && a->packet->pts <= a->last_packet_pts) {
            av_packet_unref(a->packet);
            return fail(a, "Mixed audio timestamps stopped increasing.");
        }
        a->last_packet_pts = a->packet->pts;
        enum sc_sink_result accepted = a->sink->ops->push(a->sink, a->packet);
        av_packet_unref(a->packet);
        if (accepted != SC_SINK_OK) return fail(a, "Recording could not accept mixed audio.");
    }
}

static bool
encode(struct sc_recording_audio *a, unsigned count) {
    assert(count && count <= (unsigned) a->encoder->frame_size);
    if (av_frame_make_writable(a->mixed) < 0) return fail(a, "Cannot allocate mixed audio frame.");
    a->mixed->nb_samples = count;
    a->mixed->pts = a->cursor;
    const float gain = 1.f / a->count;
    for (unsigned channel = 0; channel < CHANNELS; ++channel) {
        float *output = (float *) a->mixed->data[channel];
        for (unsigned i = 0; i < count; ++i) {
            int64_t position = a->cursor + i;
            float value = 0;
            for (unsigned s = 0; s < SC_RECORDING_AUDIO_SOURCE_COUNT; ++s) {
                if (!(a->mask & (1u << s))) continue;
                assert(contains(&a->sources[s], position));
                value += a->sources[s].samples[slot(position) * CHANNELS + channel];
            }
            output[i] = fmaxf(-1.f, fminf(1.f, value * gain));
        }
    }
    int result = avcodec_send_frame(a->encoder, a->mixed);
    if (result < 0) return fail(a, "AAC encoder rejected mixed audio.");
    a->cursor += count;
    a->progressed_at = sc_tick_now();
    return receive_encoded(a);
}

static unsigned
common_samples(struct sc_recording_audio *a, unsigned maximum) {
    for (unsigned i = 0; i < maximum; ++i) {
        for (unsigned s = 0; s < SC_RECORDING_AUDIO_SOURCE_COUNT; ++s) {
            if ((a->mask & (1u << s)) && !contains(&a->sources[s], a->cursor + i)) return i;
        }
    }
    return maximum;
}

static bool
produce(struct sc_recording_audio *a, bool final) {
    if (!a->cursor_set) {
        int64_t first = INT64_MIN;
        for (unsigned s = 0; s < SC_RECORDING_AUDIO_SOURCE_COUNT; ++s) {
            if (!(a->mask & (1u << s))) continue;
            const struct source_audio *source = &a->sources[s];
            if (!source->seen) return true;
            // During microphone startup old device samples may wrap the ring.
            int64_t available = source->end - TIMELINE_SAMPLES;
            if (available < source->first) available = source->first;
            if (first < available) first = available;
        }
        a->cursor = first;
        a->cursor_set = true;
    }
    unsigned frame_size = a->encoder->frame_size;
    for (;;) {
        unsigned count = common_samples(a, frame_size);
        if (count < frame_size && !(final && count)) return true;
        sc_mutex_lock(&a->mutex);
        a->ready = true;
        sc_mutex_unlock(&a->mutex);
        if (!encode(a, count)) return false;
    }
}

static bool
convert(struct sc_recording_audio *a, unsigned id, const struct queued_audio *item) {
    struct source_audio *source = &a->sources[id];
    const AVFrame *frame = item->frame;
    if (!source->swr) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&source->swr, &stereo, AV_SAMPLE_FMT_FLT, RATE,
                &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL) < 0
                // Enable the resampling filter even at 48k so future clock
                // compensation never has to reinitialize a bypassed filter.
                || av_opt_set_int(source->swr, "flags", SWR_FLAG_RESAMPLE, 0) < 0
                || swr_init(source->swr) < 0
                || av_channel_layout_copy(&source->layout, &frame->ch_layout) < 0) {
            return source_fail(a, id, "cannot initialize audio conversion.");
        }
        source->rate = frame->sample_rate;
        source->format = frame->format;
    } else if (source->rate != frame->sample_rate || source->format != frame->format
            || av_channel_layout_compare(&source->layout, &frame->ch_layout)) {
        return source_fail(a, id, "format changed during recording; start a new clip.");
    }
    int64_t delay = swr_get_delay(source->swr, source->rate);
    int64_t pts = item->pts - av_rescale(delay, 1000000, source->rate);
    int64_t start = av_rescale_q(pts - a->origin, micros, samples);
    if (source->seen) {
        int64_t error = start - source->end;
        if (error > MAX_CLOCK_JUMP || error < -MAX_CLOCK_JUMP) {
            return source_fail(a, id, "capture timestamps jumped or audio was lost.");
        }
        // Arrival scheduling jitter must not insert silence or discard PCM.
        // Keep sample continuity and correct sustained clock error gradually.
        // The reference is Android's monotonic/video timestamp clock, not a
        // nominal sample counter. Even the device-output hardware clock can
        // drift relative to that reference and needs the same correction.
        // Smooth phase error before correcting rate: irregular callback/packet
        // scheduling must not itself become audible pitch modulation.
        source->clock_error += (error - source->clock_error) / 32.;
        int correction = 0;
        if (source->clock_error > CLOCK_DEADBAND) correction = MAX_COMPENSATION;
        else if (source->clock_error < -CLOCK_DEADBAND) correction = -MAX_COMPENSATION;
        if (swr_set_compensation(source->swr, correction, RATE) < 0)
            return source_fail(a, id, "cannot correct capture clock drift.");
        start = source->end;
    }
    int capacity = swr_get_out_samples(source->swr, frame->nb_samples);
    if (capacity <= 0 || capacity > TIMELINE_SAMPLES) return source_fail(a, id, "invalid audio frame size.");
    float *converted = av_malloc_array((size_t) capacity * CHANNELS, sizeof(float));
    if (!converted) return fail(a, "Cannot allocate audio conversion buffer.");
    uint8_t *output = (uint8_t *) converted;
    int count = swr_convert(source->swr, &output, capacity,
                           (const uint8_t **) frame->extended_data, frame->nb_samples);
    if (count < 0) {
        av_free(converted);
        return source_fail(a, id, "audio conversion failed.");
    }
    if (a->cursor_set && start + count - a->cursor > TIMELINE_SAMPLES) {
        av_free(converted);
        return source_fail(a, id, "audio alignment buffer exceeded one second.");
    }
    if (count && !source->seen) {
        source->seen = true;
        source->first = start;
        source->end = start;
    }
    for (int i = 0; i < count; ++i) {
        int64_t p = start + i;
        if (p < source->end || (a->cursor_set && p < a->cursor)) continue;
        size_t index = slot(p);
        source->positions[index] = p;
        for (unsigned channel = 0; channel < CHANNELS; ++channel) {
            float value = converted[i * CHANNELS + channel];
            if (!isfinite(value)) {
                av_free(converted);
                return source_fail(a, id, "capture produced invalid audio samples.");
            }
            source->samples[index * CHANNELS + channel] = value;
        }
    }
    if (count) {
        if (source->end < start + count) source->end = start + count;
    }
    av_free(converted);
    return produce(a, false);
}

static struct queued_audio *
dequeue(struct sc_recording_audio *a, unsigned *id, unsigned *round_robin) {
    for (unsigned i = 0; i < SC_RECORDING_AUDIO_SOURCE_COUNT; ++i) {
        unsigned s = (*round_robin + i) % SC_RECORDING_AUDIO_SOURCE_COUNT;
        if (!a->origin_set && s != SC_RECORDING_AUDIO_DEVICE) continue;
        struct source_audio *source = &a->sources[s];
        struct queued_audio *item = source->head;
        if (!item) continue;
        source->head = item->next;
        if (!source->head) source->tail = NULL;
        --source->count;
        a->queued_bytes -= item->bytes;
        *id = s;
        *round_robin = (s + 1) % SC_RECORDING_AUDIO_SOURCE_COUNT;
        return item;
    }
    return NULL;
}

static int
run(void *userdata) {
    struct sc_recording_audio *a = userdata;
    unsigned round_robin = 0;
    for (;;) {
        unsigned source = 0;
        sc_mutex_lock(&a->mutex);
        if (a->failed) { sc_mutex_unlock(&a->mutex); break; }
        struct queued_audio *item = dequeue(a, &source, &round_robin);
        bool stopping = a->stopping;
        if (!item && !stopping) {
            sc_tick now = sc_tick_now();
            bool timed_out = a->ready ? now - a->progressed_at > STALL_TIMEOUT
                                     : now - a->started_at > STARTUP_TIMEOUT;
            if (timed_out) {
                fail_locked(a, a->ready
                    ? "A selected audio source stopped delivering synchronized samples."
                    : "A selected audio source did not become ready within three seconds.");
                sc_mutex_unlock(&a->mutex);
                break;
            }
            sc_cond_timedwait(&a->cond, &a->mutex, now + SC_TICK_FROM_MS(20));
        }
        sc_mutex_unlock(&a->mutex);
        if (!item) {
            if (stopping) break;
            continue;
        }
        if (!a->origin_set) { a->origin = item->pts; a->origin_set = true; }
        bool ok = convert(a, source, item);
        av_frame_free(&item->frame);
        free(item);
        if (!ok) break;
    }
    if (a->origin_set && !sc_recording_audio_failed(a, NULL, 0)) {
        if (produce(a, true)) {
            int result = avcodec_send_frame(a->encoder, NULL);
            if (result < 0) fail(a, "AAC encoder could not finalize the recording.");
            else receive_encoded(a);
        }
    }
    return 0;
}

struct sc_recording_audio *
sc_recording_audio_create(unsigned microphone_mask, struct sc_packet_sink *sink,
                          char *error, size_t error_size) {
    const unsigned supported = SC_RECORDING_AUDIO_ANDROID_MIC_MASK | SC_RECORDING_AUDIO_LOCAL_MIC_MASK;
    if (microphone_mask & ~supported) {
        if (error && error_size) snprintf(error, error_size, "Unknown microphone selection.");
        return NULL;
    }
    struct sc_recording_audio *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    if (!sc_mutex_init(&a->mutex)) { free(a); return NULL; }
    if (!sc_cond_init(&a->cond)) { sc_mutex_destroy(&a->mutex); free(a); return NULL; }
    a->mask = microphone_mask | (1u << SC_RECORDING_AUDIO_DEVICE);
    a->sink = sink;
    a->last_packet_pts = AV_NOPTS_VALUE;
    for (unsigned s = 0; s < SC_RECORDING_AUDIO_SOURCE_COUNT; ++s) {
        if (!(a->mask & (1u << s))) continue;
        ++a->count;
        a->sources[s].samples = av_calloc(TIMELINE_SAMPLES * CHANNELS, sizeof(float));
        a->sources[s].positions = av_malloc_array(TIMELINE_SAMPLES, sizeof(int64_t));
        if (!a->sources[s].samples || !a->sources[s].positions) goto allocation_error;
        for (unsigned i = 0; i < TIMELINE_SAMPLES; ++i) a->sources[s].positions[i] = INT64_MIN;
    }
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        snprintf(a->error, sizeof(a->error), "This build does not include the AAC audio encoder.");
        goto error;
    }
    a->encoder = avcodec_alloc_context3(codec);
    a->mixed = av_frame_alloc();
    a->packet = av_packet_alloc();
    if (!a->encoder || !a->mixed || !a->packet) goto allocation_error;
    a->encoder->sample_rate = RATE;
    a->encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&a->encoder->ch_layout, CHANNELS);
    a->encoder->time_base = samples;
    a->encoder->bit_rate = 256000;
    a->encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    a->encoder->thread_count = 1;
    if (avcodec_open2(a->encoder, codec, NULL) < 0) {
        snprintf(a->error, sizeof(a->error), "Could not initialize AAC audio encoding.");
        goto error;
    }
    a->mixed->sample_rate = RATE;
    a->mixed->format = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&a->mixed->ch_layout, CHANNELS);
    a->mixed->nb_samples = a->encoder->frame_size;
    if (av_frame_get_buffer(a->mixed, 0) < 0) goto allocation_error;
    AVCodecParameters *params = avcodec_parameters_alloc();
    if (!params) goto allocation_error;
    int result = avcodec_parameters_from_context(params, a->encoder);
    bool opened = result >= 0 && sink->ops->open(sink, params, NULL);
    avcodec_parameters_free(&params);
    if (!opened) { snprintf(a->error, sizeof(a->error), "Recording rejected mixed audio format."); goto error; }
    a->sink_open = true;
    AVPacket config = {.data = a->encoder->extradata, .size = a->encoder->extradata_size,
                       .pts = AV_NOPTS_VALUE, .dts = AV_NOPTS_VALUE};
    if (sink->ops->push(sink, &config) != SC_SINK_OK) {
        snprintf(a->error, sizeof(a->error), "Recording rejected AAC codec configuration.");
        goto error;
    }
    return a;

allocation_error:
    snprintf(a->error, sizeof(a->error), "Could not allocate mixed audio recording buffers.");
error:
    if (error && error_size) snprintf(error, error_size, "%s", a->error);
    sc_recording_audio_destroy(a);
    return NULL;
}

bool
sc_recording_audio_start(struct sc_recording_audio *a) {
    assert(!a->started);
    a->started_at = a->progressed_at = sc_tick_now();
    a->started = sc_thread_create(&a->thread, run, "recording-audio", a);
    return a->started || fail(a, "Could not start audio mixing worker.");
}

bool
sc_recording_audio_push(struct sc_recording_audio *a,
                        enum sc_recording_audio_source source,
                        const AVFrame *frame, int64_t pts_us) {
    if ((unsigned) source >= SC_RECORDING_AUDIO_SOURCE_COUNT) return fail(a, "Unknown recording audio source.");
    if (!(a->mask & (1u << source))) return true;
    if (!frame || pts_us == AV_NOPTS_VALUE || pts_us < 0
            || frame->nb_samples <= 0 || frame->sample_rate < 8000
            || frame->sample_rate > 192000 || frame->nb_samples > frame->sample_rate / 2
            || frame->ch_layout.nb_channels < 1 || frame->ch_layout.nb_channels > 2
            || av_get_bytes_per_sample(frame->format) <= 0) {
        return source_fail(a, source, "invalid PCM data or capture timestamp.");
    }
    size_t bytes = (size_t) frame->nb_samples * frame->ch_layout.nb_channels
                    * av_get_bytes_per_sample(frame->format);
    sc_mutex_lock(&a->mutex);
    if (a->stopping || a->failed) {
        bool ok = !a->failed;
        sc_mutex_unlock(&a->mutex);
        return ok;
    }
    struct source_audio *input = &a->sources[source];
    if (input->count >= MAX_QUEUED_FRAMES || bytes > MAX_QUEUED_BYTES - a->queued_bytes) {
        fail_locked(a, "Audio mixing input queue overflowed; recording stopped.");
        sc_mutex_unlock(&a->mutex);
        return false;
    }
    struct queued_audio *item = calloc(1, sizeof(*item));
    if (!item || !(item->frame = av_frame_clone(frame))) {
        free(item);
        fail_locked(a, "Could not retain recording audio samples.");
        sc_mutex_unlock(&a->mutex);
        return false;
    }
    item->pts = pts_us;
    item->bytes = bytes;
    if (input->tail) input->tail->next = item;
    else input->head = item;
    input->tail = item;
    ++input->count;
    a->queued_bytes += bytes;
    sc_cond_signal(&a->cond);
    sc_mutex_unlock(&a->mutex);
    return true;
}

bool
sc_recording_audio_ready(struct sc_recording_audio *a) {
    sc_mutex_lock(&a->mutex);
    bool ready = a->ready && !a->failed;
    sc_mutex_unlock(&a->mutex);
    return ready;
}

bool
sc_recording_audio_failed(struct sc_recording_audio *a, char *error, size_t size) {
    sc_mutex_lock(&a->mutex);
    bool failed = a->failed;
    if (error && size) snprintf(error, size, "%s", failed ? a->error : "");
    sc_mutex_unlock(&a->mutex);
    return failed;
}

void
sc_recording_audio_stop(struct sc_recording_audio *a) {
    sc_mutex_lock(&a->mutex);
    a->stopping = true;
    sc_cond_broadcast(&a->cond);
    sc_mutex_unlock(&a->mutex);
}

void
sc_recording_audio_join(struct sc_recording_audio *a) {
    if (a->started && !a->joined) {
        sc_thread_join(&a->thread, NULL);
        a->joined = true;
    }
}

void
sc_recording_audio_destroy(struct sc_recording_audio *a) {
    if (!a) return;
    sc_recording_audio_stop(a);
    sc_recording_audio_join(a);
    if (a->sink_open) a->sink->ops->close(a->sink);
    for (unsigned s = 0; s < SC_RECORDING_AUDIO_SOURCE_COUNT; ++s) {
        struct source_audio *source = &a->sources[s];
        while (source->head) {
            struct queued_audio *item = source->head;
            source->head = item->next;
            av_frame_free(&item->frame);
            free(item);
        }
        swr_free(&source->swr);
        av_channel_layout_uninit(&source->layout);
        av_free(source->samples);
        av_free(source->positions);
    }
    av_frame_free(&a->mixed);
    av_packet_free(&a->packet);
    avcodec_free_context(&a->encoder);
    sc_cond_destroy(&a->cond);
    sc_mutex_destroy(&a->mutex);
    free(a);
}
