#ifndef SC_RECORDING_CLOCK_H
#define SC_RECORDING_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define SC_RECORDING_CLOCK_SLOTS 8
#define SC_RECORDING_CLOCK_MAX_RTT_US INT64_C(50000)
#define SC_RECORDING_CLOCK_EXPIRY_US INT64_C(10000000)

struct sc_recording_clock_pending {
    bool valid;
    uint32_t sequence;
    int64_t sent_us;
};

struct sc_recording_clock_sample {
    bool valid;
    int64_t received_us;
    int64_t rtt_us;
    int64_t offset_us;
};

// No internal locking. The caller serializes begin/complete/map/reset and uses
// one instance per recording session. Neither API opens a microphone or device.
struct sc_recording_clock {
    struct sc_recording_clock_pending pending[SC_RECORDING_CLOCK_SLOTS];
    struct sc_recording_clock_sample samples[SC_RECORDING_CLOCK_SLOTS];
    unsigned pending_next;
    unsigned sample_next;
    bool calibrated;
    bool mapped;
    int64_t offset_us;
    int64_t target_offset_us;
    int64_t best_rtt_us;
    int64_t updated_us;
    unsigned slew_remainder_us;
    int64_t last_host_pts;
    int64_t last_device_pts;
};

void sc_recording_clock_reset(struct sc_recording_clock *clock);
bool sc_recording_clock_begin(struct sc_recording_clock *clock,
                              uint32_t sequence, int64_t host_send_us);
bool sc_recording_clock_complete(struct sc_recording_clock *clock,
                                 uint32_t sequence, int64_t device_us,
                                 int64_t host_receive_us);
bool sc_recording_clock_ready(const struct sc_recording_clock *clock,
                              int64_t now_host_us);
// Rejects an expired calibration, non-increasing input/output, or overflow.
bool sc_recording_clock_map(struct sc_recording_clock *clock, int64_t host_pts,
                            int64_t now_host_us, int64_t *device_pts);
// Transport midpoint uncertainty plus outstanding offset slew. This does not
// measure the physical microphone's hardware/driver capture latency.
int64_t sc_recording_clock_uncertainty_us(const struct sc_recording_clock *clock);

#endif
