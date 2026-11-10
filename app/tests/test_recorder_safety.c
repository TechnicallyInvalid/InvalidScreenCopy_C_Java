// Headless regressions: a stalled output must not block stream producers or
// consume unbounded memory, and late audio startup must not touch a freed muxer.
#include "recorder.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

struct blocked_output {
    sc_mutex mutex;
    sc_cond cond;
    bool entered;
    bool released;
    fpos_t position;
};

static int
write_blocked(void *cookie, const char *bytes, int length) {
    (void) bytes;
    struct blocked_output *output = cookie;
    sc_mutex_lock(&output->mutex);
    output->entered = true;
    sc_cond_signal(&output->cond);
    while (!output->released) {
        sc_cond_wait(&output->cond, &output->mutex);
    }
    output->position += length;
    sc_mutex_unlock(&output->mutex);
    return length;
}

static fpos_t
seek_output(void *cookie, fpos_t offset, int whence) {
    struct blocked_output *output = cookie;
    if (whence == SEEK_SET) {
        output->position = offset;
    } else {
        assert(whence == SEEK_CUR);
        output->position += offset;
    }
    return output->position;
}

static void
ended(struct sc_recorder *recorder, bool success, void *userdata) {
    (void) recorder;
    *(bool *) userdata = success;
}

static const struct sc_recorder_callbacks callbacks = {.on_ended = ended};

static AVCodecParameters *
audio_parameters(void) {
    AVCodecParameters *params = avcodec_parameters_alloc();
    assert(params);
    params->codec_type = AVMEDIA_TYPE_AUDIO;
    params->codec_id = AV_CODEC_ID_PCM_S16LE;
    params->sample_rate = 48000;
    params->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
    params->bits_per_coded_sample = 16;
    return params;
}

static void
test_queue_limit(bool byte_limit) {
    struct blocked_output output = {0};
    assert(sc_mutex_init(&output.mutex));
    assert(sc_cond_init(&output.cond));
    struct sc_recorder recorder;
    bool success = true;
    assert(sc_recorder_init(&recorder, "stalled-test-output", SC_RECORD_FORMAT_WAV,
                            false, true, SC_ORIENTATION_0, &callbacks, &success));
    recorder.output_file = funopen(&output, NULL, write_blocked, seek_output, NULL);
    assert(recorder.output_file);
    assert(!setvbuf(recorder.output_file, NULL, _IONBF, 0));
    assert(sc_recorder_start(&recorder));
    AVCodecParameters *params = audio_parameters();
    struct sc_packet_sink *sink = &recorder.audio_packet_sink;
    assert(sink->ops->open(sink, params, NULL));
    avcodec_parameters_free(&params);

    AVPacket *packet = av_packet_alloc();
    assert(packet && !av_new_packet(packet, 65536));
    memset(packet->data, 0, packet->size);
    packet->pts = packet->dts = 0;
    packet->duration = 65536 * INT64_C(1000000) / (48000 * 4);
    assert(sink->ops->push(sink, packet) == SC_SINK_OK);

    // A single PCM packet flushes AVIO's 32 KiB buffer into the blocked sink.
    sc_mutex_lock(&output.mutex);
    sc_tick deadline = sc_tick_now() + SC_TICK_FROM_SEC(5);
    while (!output.entered) {
        bool signaled = sc_cond_timedwait(&output.cond, &output.mutex, deadline);
        assert(signaled || output.entered);
    }
    sc_mutex_unlock(&output.mutex);
    sc_mutex_lock(&recorder.mutex);
    size_t initial_packets = sc_vecdeque_size(&recorder.audio_queue);
    sc_mutex_unlock(&recorder.mutex);

    if (!byte_limit) {
        av_packet_unref(packet);
        assert(!av_new_packet(packet, 4));
        memset(packet->data, 0, packet->size);
        packet->duration = 21;
    }
    packet->pts = packet->dts = 400000;
    enum sc_sink_result result = SC_SINK_OK;
    size_t accepted = 0;
    while (result == SC_SINK_OK) {
        result = sink->ops->push(sink, packet);
        if (result == SC_SINK_OK) ++accepted;
        packet->pts += packet->duration;
        packet->dts = packet->pts;
        assert(accepted <= SC_RECORDER_MAX_QUEUE_PACKETS);
    }
    assert(result == SC_SINK_KO);
    assert(byte_limit ? accepted + initial_packets < SC_RECORDER_MAX_QUEUE_PACKETS
                      : accepted + initial_packets == SC_RECORDER_MAX_QUEUE_PACKETS);
    sc_mutex_lock(&recorder.mutex);
    assert(recorder.stopped && recorder.queue_overflow);
    assert(recorder.queued_bytes <= SC_RECORDER_MAX_QUEUE_BYTES);
    sc_mutex_unlock(&recorder.mutex);
    assert(sink->ops->push(sink, packet) == SC_SINK_STOPPED);
    av_packet_free(&packet);

    sc_mutex_lock(&output.mutex);
    output.released = true;
    sc_cond_signal(&output.cond);
    sc_mutex_unlock(&output.mutex);
    sc_recorder_join(&recorder);
    // A successfully finalized partial WAV still represents failed recording.
    assert(!success);
    assert(!recorder.ctx && !recorder.output_file);
    assert(!recorder.queued_bytes);
    assert(sc_vecdeque_is_empty(&recorder.audio_queue));
    sc_recorder_destroy(&recorder);
    sc_cond_destroy(&output.cond);
    sc_mutex_destroy(&output.mutex);
}

static void
test_late_audio_open(void) {
    struct sc_recorder recorder;
    bool success = true;
    assert(sc_recorder_init(&recorder, "late-audio-test", SC_RECORD_FORMAT_MP4,
                            true, true, SC_ORIENTATION_0, &callbacks, &success));
    recorder.output_file = tmpfile();
    assert(recorder.output_file);
    assert(sc_recorder_start(&recorder));
    sc_recorder_stop(&recorder);
    sc_recorder_join(&recorder);
    assert(!success && !recorder.ctx);
    AVCodecParameters *params = audio_parameters();
    struct sc_packet_sink *sink = &recorder.audio_packet_sink;
    assert(!sink->ops->open(sink, params, NULL));
    avcodec_parameters_free(&params);
    sc_recorder_destroy(&recorder);
}

int
main(void) {
    assert(SDL_Init(SDL_INIT_EVENTS));
    test_queue_limit(false);
    test_queue_limit(true);
    test_late_audio_open();
    SDL_Quit();
    return 0;
}
