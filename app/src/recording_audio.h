#ifndef SC_RECORDING_AUDIO_H
#define SC_RECORDING_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <libavutil/frame.h>
#include "trait/packet_sink.h"

enum sc_recording_audio_source {
    SC_RECORDING_AUDIO_DEVICE,
    SC_RECORDING_AUDIO_ANDROID_MIC,
    SC_RECORDING_AUDIO_LOCAL_MIC,
    SC_RECORDING_AUDIO_SOURCE_COUNT,
};

#define SC_RECORDING_AUDIO_ANDROID_MIC_MASK (1u << SC_RECORDING_AUDIO_ANDROID_MIC)
#define SC_RECORDING_AUDIO_LOCAL_MIC_MASK (1u << SC_RECORDING_AUDIO_LOCAL_MIC)

struct sc_recording_audio;

// Called on the recording lifecycle worker. Opens an AAC encoder and the sink,
// and sends AudioSpecificConfig. Device output is always included. No capture
// device or permission API is called here. The sink must outlive destroy().
struct sc_recording_audio *
sc_recording_audio_create(unsigned microphone_mask, struct sc_packet_sink *sink,
                          char *error, size_t error_size);
bool sc_recording_audio_start(struct sc_recording_audio *audio);

// Explicit timestamps refer to the FIRST input sample, in the Android video /
// device-output microsecond clock. Callers map local capture timestamps into
// that clock before pushing. Frames are referenced, never retained by pointer.
// Nonselected inputs and inputs after a normal stop are ignored. false means
// a recording error, never permission to silently omit that selected source.
bool sc_recording_audio_push(struct sc_recording_audio *audio,
                             enum sc_recording_audio_source source,
                             const AVFrame *frame, int64_t pts_us);
bool sc_recording_audio_ready(struct sc_recording_audio *audio);
bool sc_recording_audio_failed(struct sc_recording_audio *audio,
                               char *error, size_t error_size);
// Stop is nonblocking. Join/destroy belong to the lifecycle worker, never UI or
// producer callbacks. Stop drains only the common available source interval.
void sc_recording_audio_stop(struct sc_recording_audio *audio);
void sc_recording_audio_join(struct sc_recording_audio *audio);
void sc_recording_audio_destroy(struct sc_recording_audio *audio);

#endif
