#include "common.h"
#include "recording_audio.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <SDL3/SDL.h>

#include "util/thread.h"

#define RATE 48000
#define BASE_US INT64_C(9000000000)
#define MAX_PACKETS 4096

struct output {
    struct sc_packet_sink sink;
    AVCodecParameters *params;
    AVPacket *packets[MAX_PACKETS];
    unsigned count;
    bool reject_open;
    bool reject_packets;
    bool opened;
    bool closed;
    int64_t decoded_first_pts;
    sc_mutex mutex;
    sc_cond cond;
};

static bool
output_open(struct sc_packet_sink *sink, const AVCodecParameters *params,
             const struct sc_stream_session *session) {
    (void) session;
    struct output *out = container_of(sink, struct output, sink);
    if (out->reject_open) return false;
    out->params = avcodec_parameters_alloc();
    assert(out->params && avcodec_parameters_copy(out->params, params) >= 0);
    out->opened = true;
    return true;
}

static void
output_close(struct sc_packet_sink *sink) {
    struct output *out = container_of(sink, struct output, sink);
    assert(out->opened && !out->closed);
    out->closed = true;
}

static enum sc_sink_result
output_push(struct sc_packet_sink *sink, const AVPacket *packet) {
    struct output *out = container_of(sink, struct output, sink);
    if (packet->pts == AV_NOPTS_VALUE) {
        assert(packet->size > 0 && packet->size == out->params->extradata_size);
        assert(!memcmp(packet->data, out->params->extradata, packet->size));
        return SC_SINK_OK;
    }
    if (out->reject_packets) return SC_SINK_KO;
    sc_mutex_lock(&out->mutex);
    assert(out->count < MAX_PACKETS);
    if (out->count) {
        assert(packet->pts > out->packets[out->count - 1]->pts);
        assert(packet->dts > out->packets[out->count - 1]->dts);
    }
    assert(packet->duration > 0);
    out->packets[out->count++] = av_packet_clone(packet);
    assert(out->packets[out->count - 1]);
    sc_cond_broadcast(&out->cond);
    sc_mutex_unlock(&out->mutex);
    return SC_SINK_OK;
}

static void
init_output(struct output *out) {
    memset(out, 0, sizeof(*out));
    static const struct sc_packet_sink_ops ops = {
        .open = output_open, .close = output_close, .push = output_push,
    };
    out->sink.ops = &ops;
    assert(sc_mutex_init(&out->mutex));
    assert(sc_cond_init(&out->cond));
}

static void
destroy_output(struct output *out) {
    for (unsigned i = 0; i < out->count; ++i) av_packet_free(&out->packets[i]);
    avcodec_parameters_free(&out->params);
    sc_cond_destroy(&out->cond);
    sc_mutex_destroy(&out->mutex);
}

static AVFrame *
tone(unsigned source, unsigned block, int rate) {
    AVFrame *frame = av_frame_alloc();
    assert(frame);
    frame->sample_rate = rate;
    frame->nb_samples = rate / 50;
    frame->format = source == SC_RECORDING_AUDIO_DEVICE ? AV_SAMPLE_FMT_FLTP
                  : source == SC_RECORDING_AUDIO_ANDROID_MIC ? AV_SAMPLE_FMT_S16
                  : AV_SAMPLE_FMT_FLT;
    av_channel_layout_default(&frame->ch_layout, source == SC_RECORDING_AUDIO_DEVICE ? 2 : 1);
    assert(av_frame_get_buffer(frame, 0) >= 0);
    const double frequencies[] = {440, 997, 1703};
    for (int i = 0; i < frame->nb_samples; ++i) {
        int64_t index = (int64_t) block * frame->nb_samples + i;
        float value = .18f * sin(2.0 * M_PI * frequencies[source] * index / rate);
        if (frame->format == AV_SAMPLE_FMT_FLTP) {
            ((float *) frame->data[0])[i] = value;
            ((float *) frame->data[1])[i] = value;
        } else if (frame->format == AV_SAMPLE_FMT_FLT) {
            ((float *) frame->data[0])[i] = value;
        } else {
            ((int16_t *) frame->data[0])[i] = (int16_t) lrintf(value * 32767);
        }
    }
    return frame;
}

static void
feed(struct sc_recording_audio *audio, unsigned source, unsigned block,
      int rate, int64_t offset) {
    AVFrame *frame = tone(source, block, rate);
    bool ok = sc_recording_audio_push(audio, source, frame,
                                      BASE_US + block * INT64_C(20000) + offset);
    av_frame_free(&frame);
    if (!ok) {
        char error[256];
        sc_recording_audio_failed(audio, error, sizeof(error));
        fprintf(stderr, "push failed: %s\n", error);
    }
    assert(ok);
}

static unsigned
decode(struct output *out, float **pcm) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    assert(codec);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    assert(ctx && avcodec_parameters_to_context(ctx, out->params) >= 0);
    ctx->pkt_timebase = (AVRational) {1, 1000000};
    assert(avcodec_open2(ctx, codec, NULL) >= 0);
    AVFrame *frame = av_frame_alloc();
    assert(frame);
    unsigned count = 0;
    *pcm = malloc(RATE * 90 * sizeof(float));
    assert(*pcm);
    for (unsigned i = 0; i <= out->count; ++i) {
        assert(avcodec_send_packet(ctx, i < out->count ? out->packets[i] : NULL) >= 0);
        int result;
        while (!(result = avcodec_receive_frame(ctx, frame))) {
            assert(frame->sample_rate == RATE && frame->format == AV_SAMPLE_FMT_FLTP);
            assert(frame->ch_layout.nb_channels == 2);
            assert(count + frame->nb_samples <= RATE * 90);
            if (!count) out->decoded_first_pts = frame->pts;
            memcpy(*pcm + count, frame->data[0], frame->nb_samples * sizeof(float));
            count += frame->nb_samples;
            av_frame_unref(frame);
        }
        assert(result == AVERROR(EAGAIN) || result == AVERROR_EOF);
    }
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return count;
}

static double
amplitude(const float *pcm, unsigned count, double frequency) {
    double sine = 0, cosine = 0;
    unsigned start = RATE / 4;
    unsigned end = count - RATE / 4;
    assert(end > start);
    for (unsigned i = start; i < end; ++i) {
        double angle = 2 * M_PI * frequency * i / RATE;
        sine += pcm[i] * sin(angle);
        cosine += pcm[i] * cos(angle);
    }
    return 2 * hypot(sine, cosine) / (end - start);
}

static void
check_combination(unsigned mask, bool jitter) {
    struct output out;
    init_output(&out);
    char error[256] = "";
    struct sc_recording_audio *audio = sc_recording_audio_create(mask, &out.sink, error, sizeof(error));
    if (!audio) fprintf(stderr, "create failed: %s\n", error);
    assert(audio);
    for (unsigned i = 0; i < 100; ++i) {
        // Disabled sources are deliberately supplied too: they must remain
        // absent from encoded audio, not merely be hidden by the UI toggles.
        feed(audio, SC_RECORDING_AUDIO_DEVICE, i, RATE, 0);
        feed(audio, SC_RECORDING_AUDIO_ANDROID_MIC, i, RATE, 0);
        int64_t shift = jitter && i ? (i % 3 == 0 ? 10000 : i % 3 == 1 ? -10000 : 0) : 0;
        feed(audio, SC_RECORDING_AUDIO_LOCAL_MIC, i, 44100, shift);
    }
    assert(sc_recording_audio_start(audio));
    sc_recording_audio_stop(audio);
    sc_recording_audio_join(audio);
    if (sc_recording_audio_failed(audio, error, sizeof(error))) fprintf(stderr, "%s\n", error);
    assert(!sc_recording_audio_failed(audio, NULL, 0));
    assert(sc_recording_audio_ready(audio));
    assert(out.params->codec_id == AV_CODEC_ID_AAC && out.params->sample_rate == RATE);
    assert(out.count > 90 && out.count < 100);
    assert(llabs(out.packets[0]->pts - (BASE_US - 21333)) < 2);
    int64_t end = out.packets[out.count - 1]->pts + out.packets[out.count - 1]->duration;
    assert(llabs(end - (BASE_US + 2000000)) < 25000);
    sc_recording_audio_destroy(audio);
    assert(out.closed);
    float *pcm;
    unsigned count = decode(&out, &pcm);
    assert(count > RATE * 1.98 && count < RATE * 2.05);
    const double frequencies[] = {440, 997, 1703};
    for (unsigned source = 0; source < 3; ++source) {
        double measured = amplitude(pcm, count, frequencies[source]);
        // Bounded clock correction can move a tone by up to 0.1%; measure its
        // narrow neighboring band rather than expecting exact Fourier phase.
        if (jitter && source == SC_RECORDING_AUDIO_LOCAL_MIC) {
            for (int bin = -8; bin <= 8; ++bin) {
                double nearby = amplitude(pcm, count, frequencies[source] + bin * .5);
                if (nearby > measured) measured = nearby;
            }
        }
        bool selected = !source || (mask & (1u << source));
        if (!(selected ? measured > .025 : measured < .003))
            fprintf(stderr, "mask %u source %u selected=%d amplitude %.6f\n", mask, source, selected, measured);
        assert(selected ? measured > .025 : measured < .003);
    }
    for (unsigned i = 0; i < count; ++i) assert(isfinite(pcm[i]) && fabsf(pcm[i]) < .5f);
    free(pcm);
    destroy_output(&out);
    printf("PASS: microphone mask %u, all selected tones present, disabled tones absent, AAC timestamps/duration%s\n", mask, jitter ? ", jitter and resampling" : "");
}

static void
check_drift(bool device_clock_drift) {
    struct output out;
    init_output(&out);
    char error[256] = "";
    struct sc_recording_audio *audio = sc_recording_audio_create(
        SC_RECORDING_AUDIO_LOCAL_MIC_MASK, &out.sink, error, sizeof(error));
    assert(audio && sc_recording_audio_start(audio));
    for (unsigned i = 0; i < 3000; ++i) {
        feed(audio, SC_RECORDING_AUDIO_DEVICE, i, RATE, device_clock_drift ? i * INT64_C(4) : 0);
        AVFrame *frame = tone(SC_RECORDING_AUDIO_LOCAL_MIC, i, RATE);
        // A 100ms mic-only tone marker every nominal second. Its sample clock
        // differs by +200ppm from the Android/video timestamp clock.
        if (i % 50 >= 5) memset(frame->data[0], 0, frame->nb_samples * sizeof(float));
        int local_interval = device_clock_drift ? 19996 : 20004;
        assert(sc_recording_audio_push(audio, SC_RECORDING_AUDIO_LOCAL_MIC,
            frame, BASE_US + i * (int64_t) local_interval));
        av_frame_free(&frame);
        if (i % 25 == 24) {
            // Synthetic producer throttling, not a clock used by the mixer.
            // Keep queued data comfortably within its real-time memory bound.
            unsigned expected = (i + 1) * 960 / 1024 - 3;
            sc_mutex_lock(&out.mutex);
            sc_tick deadline = sc_tick_now() + SC_TICK_FROM_SEC(5);
            while (out.count < expected) assert(sc_cond_timedwait(&out.cond, &out.mutex, deadline));
            sc_mutex_unlock(&out.mutex);
        }
    }
    sc_recording_audio_stop(audio);
    sc_recording_audio_join(audio);
    if (sc_recording_audio_failed(audio, error, sizeof(error))) fprintf(stderr, "%s\n", error);
    assert(!sc_recording_audio_failed(audio, NULL, 0));
    sc_recording_audio_destroy(audio);
    float *pcm;
    unsigned count = decode(&out, &pcm);
    assert(count > RATE * 59.98 && count < RATE * 60.05);
    double expected = 59.0 * (device_clock_drift ? .9998 : 1.0002);
    int detected = -1;
    // Honor decoder-reported timestamps: AAC skip-sample side data may already
    // remove encoder priming, so blindly subtracting 1024 samples is incorrect.
    double decoded_start = (out.decoded_first_pts - BASE_US) / 1000000.;
    int center = (int) llround((expected - decoded_start) * RATE);
    for (int start = center - RATE / 25; start < center + RATE / 25; start += 48) {
        double real = 0, imaginary = 0;
        for (int j = 0; j < 480; ++j) {
            double angle = 2 * M_PI * 1703 * (start + j) / RATE;
            real += pcm[start + j] * cos(angle);
            imaginary += pcm[start + j] * sin(angle);
        }
        if (2 * hypot(real, imaginary) / 480 > .025) { detected = start + 240; break; }
    }
    assert(detected >= 0);
    double measured = decoded_start + detected / (double) RATE;
    if (fabs(measured - expected) >= .006) fprintf(stderr, "drift marker expected %.6f, measured %.6f\n", expected, measured);
    assert(fabs(measured - expected) < .006);
    int64_t end = out.packets[out.count - 1]->pts + out.packets[out.count - 1]->duration;
    assert(llabs(end - (BASE_US + 60000000)) < 25000);
    free(pcm);
    destroy_output(&out);
    puts(device_clock_drift
        ? "PASS: 60-second opposing device +200ppm / mic -200ppm clocks, final marker within 6ms of video"
        : "PASS: 60-second +200ppm mic clock drift; final marker within 6ms of Android/video clock");
}

static void
check_failures(void) {
    struct output out;
    init_output(&out);
    out.reject_open = true;
    char error[256] = "";
    assert(!sc_recording_audio_create(0, &out.sink, error, sizeof(error)));
    assert(strstr(error, "format"));
    destroy_output(&out);

    init_output(&out);
    struct sc_recording_audio *audio = sc_recording_audio_create(SC_RECORDING_AUDIO_LOCAL_MIC_MASK, &out.sink, error, sizeof(error));
    assert(audio);
    // Fill before the worker starts: verifies a deterministic memory bound
    // without timing a real capture callback or stalling a physical device.
    AVFrame *frame = tone(SC_RECORDING_AUDIO_DEVICE, 0, RATE);
    unsigned accepted = 0;
    while (sc_recording_audio_push(audio, SC_RECORDING_AUDIO_DEVICE, frame, BASE_US + accepted * 20000)) ++accepted;
    assert(accepted == 128);
    assert(sc_recording_audio_failed(audio, error, sizeof(error)) && strstr(error, "queue"));
    av_frame_free(&frame);
    sc_recording_audio_destroy(audio);
    destroy_output(&out);

    init_output(&out);
    out.reject_packets = true;
    audio = sc_recording_audio_create(0, &out.sink, error, sizeof(error));
    assert(audio);
    for (unsigned i = 0; i < 5; ++i) feed(audio, SC_RECORDING_AUDIO_DEVICE, i, RATE, 0);
    assert(sc_recording_audio_start(audio));
    sc_recording_audio_stop(audio);
    sc_recording_audio_join(audio);
    assert(sc_recording_audio_failed(audio, error, sizeof(error)) && strstr(error, "accept"));
    sc_recording_audio_destroy(audio);
    destroy_output(&out);

    init_output(&out);
    audio = sc_recording_audio_create(SC_RECORDING_AUDIO_ANDROID_MIC_MASK, &out.sink, error, sizeof(error));
    assert(audio && sc_recording_audio_start(audio));
    feed(audio, SC_RECORDING_AUDIO_DEVICE, 0, RATE, 0);
    // No selected Android mic frames arrive. This is a failure, never a
    // successful recording that quietly contains device output alone.
    sc_tick deadline = sc_tick_now() + SC_TICK_FROM_SEC(4);
    while (!sc_recording_audio_failed(audio, error, sizeof(error)) && sc_tick_now() < deadline) SDL_Delay(10);
    assert(sc_recording_audio_failed(audio, error, sizeof(error)) && strstr(error, "ready"));
    assert(!out.count && !sc_recording_audio_ready(audio));
    sc_recording_audio_destroy(audio);
    destroy_output(&out);
    puts("PASS: open/encode errors, bounded input, missing selected source and shutdown");
}

int
main(void) {
    assert(SDL_Init(SDL_INIT_EVENTS));
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        for (unsigned i = 0; i < 4; ++i) {
            unsigned mask = (i & 1 ? SC_RECORDING_AUDIO_ANDROID_MIC_MASK : 0)
                          | (i & 2 ? SC_RECORDING_AUDIO_LOCAL_MIC_MASK : 0);
            check_combination(mask, iteration != 0);
        }
    }
    check_drift(false);
    check_drift(true);
    check_failures();
    SDL_Quit();
    return 0;
}
