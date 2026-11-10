// Integration harness: run with scripts/test-recording.sh (real H.264 + AAC).
#include "recording.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

static void
configure(struct sc_packet_sink *video, struct sc_packet_sink *audio,
           const char *fixture) {
    AVFormatContext *ctx = NULL;
    assert(!avformat_open_input(&ctx, fixture, NULL, NULL));
    assert(avformat_find_stream_info(ctx, NULL) >= 0);
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        AVCodecParameters *params = ctx->streams[i]->codecpar;
        struct sc_packet_sink *sink = params->codec_type == AVMEDIA_TYPE_VIDEO ? video : audio;
        if (!sink) continue;
        if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
            // Match demuxer.c: the device wire format supplies the codec and
            // fixed audio format, not the MP4's frame size or codec profile.
            AVCodecParameters *wire = avcodec_parameters_alloc();
            assert(wire);
            wire->codec_type = AVMEDIA_TYPE_AUDIO;
            wire->codec_id = params->codec_id;
            wire->sample_rate = 48000;
            wire->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
            assert(sink->ops->open(sink, wire, NULL));
            avcodec_parameters_free(&wire);
        } else {
            assert(sink->ops->open(sink, params, NULL));
        }
        if (params->extradata_size) {
            AVPacket *config = av_packet_alloc();
            assert(config && !av_new_packet(config, params->extradata_size));
            memcpy(config->data, params->extradata, params->extradata_size);
            config->pts = AV_NOPTS_VALUE;
            sink->ops->push(sink, config);
            av_packet_free(&config);
        }
    }
    avformat_close_input(&ctx);
}

static void
feed(struct sc_packet_sink *video, struct sc_packet_sink *audio,
      const char *fixture) {
    AVFormatContext *ctx = NULL;
    assert(!avformat_open_input(&ctx, fixture, NULL, NULL));
    assert(avformat_find_stream_info(ctx, NULL) >= 0);
    AVPacket *packet = av_packet_alloc();
    while (av_read_frame(ctx, packet) >= 0) {
        AVStream *stream = ctx->streams[packet->stream_index];
        struct sc_packet_sink *sink = stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? video : audio;
        if (!sink) {
            av_packet_unref(packet);
            continue;
        }
        av_packet_rescale_ts(packet, stream->time_base, (AVRational){1, 1000000});
        // Android sends presentation timestamps but no packet durations.
        packet->duration = 0;
        sink->ops->push(sink, packet);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&ctx);
}

static int
fail_write(void *cookie, const char *bytes, int length) {
    (void)cookie; (void)bytes; (void)length;
    errno = ENOSPC;
    return -1;
}

static void
ended(struct sc_recorder *recorder, bool success, void *userdata) {
    (void)recorder;
    *(bool *)userdata = success;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    assert(SDL_Init(SDL_INIT_EVENTS));
    struct sc_recording r;
    assert(sc_recording_init(&r, true, SC_ORIENTATION_0));
    configure(&r.video.sink, &r.audio.sink, argv[1]);
    feed(&r.video.sink, &r.audio.sink, argv[1]);
    for (int i = 0; i < 3; ++i) {
        assert(sc_recording_start(&r, argv[2], 1794100000123));
        AVPacket *nonkey = av_packet_alloc();
        assert(nonkey && !av_new_packet(nonkey, 8));
        nonkey->pts = nonkey->dts = 0;
        assert(r.video.sink.ops->push(&r.video.sink, nonkey) == SC_SINK_OK);
        assert(r.audio.sink.ops->push(&r.audio.sink, nonkey) == SC_SINK_OK);
        assert(!r.has_frame); // Neither stream may enter the clip before a keyframe.
        av_packet_free(&nonkey);
        feed(&r.video.sink, &r.audio.sink, argv[1]);
        if (i == 2) {
            // Simulate holding a static final frame for three seconds.
            r.first_frame_time -= 3000000;
        }
        sc_recording_stop(&r);
        assert(strstr(r.message, "Saved:"));
    }
    assert(sc_recording_start(&r, argv[2], 1794100000999));
    sc_recording_stop(&r);
    assert(strstr(r.message, "No keyframe received"));
    assert(!sc_recording_start(&r, "/no/such/recording/directory", 1794100000999));
    assert(strstr(r.message, "Cannot create"));
    // End of stream while recording must still finalize the clip.
    assert(sc_recording_start(&r, argv[2], 1794100002999));
    feed(&r.video.sink, &r.audio.sink, argv[1]);
    r.video.sink.ops->close(&r.video.sink);
    sc_recording_stop(&r);
    assert(strstr(r.message, "Saved:"));
    sc_recording_destroy(&r);

    assert(sc_recording_init(&r, false, SC_ORIENTATION_0));
    configure(&r.video.sink, NULL, argv[1]);
    assert(sc_recording_start(&r, argv[2], 1794100003999));
    feed(&r.video.sink, NULL, argv[1]);
    r.first_frame_time -= 3000000;
    sc_recording_stop(&r);
    assert(strstr(r.message, "Saved:"));
    sc_recording_destroy(&r);

    // Exercise a full-disk error through the actual AVIO write callback.
    struct sc_recorder rec;
    bool success = true;
    static const struct sc_recorder_callbacks callbacks = {.on_ended = ended};
    assert(sc_recorder_init(&rec, "injected-write-failure", SC_RECORD_FORMAT_MP4,
        true, true, SC_ORIENTATION_0, &callbacks, &success));
    rec.output_file = funopen(NULL, NULL, fail_write, NULL, NULL);
    assert(rec.output_file);
    assert(sc_recorder_start(&rec));
    configure(&rec.video_packet_sink, &rec.audio_packet_sink, argv[1]);
    feed(&rec.video_packet_sink, &rec.audio_packet_sink, argv[1]);
    sc_recorder_stop(&rec);
    sc_recorder_join(&rec);
    assert(!success);
    sc_recorder_destroy(&rec);
    SDL_Quit();
    return 0;
}
