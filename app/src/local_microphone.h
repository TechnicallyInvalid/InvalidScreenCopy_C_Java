#ifndef SC_LOCAL_MICROPHONE_H
#define SC_LOCAL_MICROPHONE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum sc_local_microphone_permission {
    SC_LOCAL_MIC_NOT_DETERMINED,
    SC_LOCAL_MIC_AUTHORIZED,
    SC_LOCAL_MIC_DENIED,
    SC_LOCAL_MIC_RESTRICTED,
};

struct sc_local_microphone;

// Permission lookup does not open hardware or prompt. Permission requests are
// deliberately a separate API, called only by an explicit Local Mic activation.
enum sc_local_microphone_permission sc_local_microphone_permission(void);
#ifdef __OBJC__
void sc_local_microphone_request_permission(
    void (^completion)(enum sc_local_microphone_permission));
#endif

// Called from SDL's capture thread: copy/queue promptly, never stop/destroy the
// adapter inside a callback. Samples are 48 kHz mono float32. pts_us estimates
// the first sample's host monotonic time (av_gettime_relative clock), compensating
// for queued samples, then advances a sample counter with robust, bounded
// correction for clock drift. SDL does not expose the hardware ADC timestamp;
// physical input latency and device-clock accuracy remain unmeasured.
struct sc_local_microphone_callbacks {
    bool (*on_pcm)(void *userdata, const float *samples, size_t count,
                   int64_t pts_us);
    void (*on_error)(void *userdata, const char *message);
};

// Only the recording lifecycle worker may start/stop. selected=false never
// queries permission or opens a microphone. The caller initializes SDL audio
// on the main thread before starting recordings (no capture device is opened).
// start() never
// prompts: authorization must already have been granted by the explicit UI.
struct sc_local_microphone *
sc_local_microphone_start(bool selected,
    const struct sc_local_microphone_callbacks *callbacks, void *userdata,
    char *error, size_t error_size);

// Detects capture starvation/disconnection or permission revocation. Call while
// a selected recording is active; false carries a visible, actionable error.
bool sc_local_microphone_poll(struct sc_local_microphone *microphone,
                              char *error, size_t error_size);
// Stops capture, waits for any callback to finish, then closes the device.
// Must run outside recording/mixer locks and before callback userdata is freed.
void sc_local_microphone_stop(struct sc_local_microphone *microphone);

#endif
