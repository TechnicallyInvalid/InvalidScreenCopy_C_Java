#include "recording_clock.h"

#include <limits.h>
#include <string.h>

static bool
subtract(int64_t a, int64_t b, int64_t *difference) {
    if ((b > 0 && a < INT64_MIN + b)
            || (b < 0 && a > INT64_MAX + b)) return false;
    *difference = a - b;
    return true;
}

void
sc_recording_clock_reset(struct sc_recording_clock *clock) {
    memset(clock, 0, sizeof(*clock));
}

bool
sc_recording_clock_begin(struct sc_recording_clock *clock, uint32_t sequence,
                          int64_t host_send_us) {
    if (host_send_us < 0) return false;
    for (unsigned i = 0; i < SC_RECORDING_CLOCK_SLOTS; ++i) {
        if (clock->pending[i].valid && clock->pending[i].sequence == sequence)
            return false;
    }
    struct sc_recording_clock_pending *pending = &clock->pending[clock->pending_next];
    *pending = (struct sc_recording_clock_pending) {
        .valid = true, .sequence = sequence, .sent_us = host_send_us,
    };
    clock->pending_next = (clock->pending_next + 1) % SC_RECORDING_CLOCK_SLOTS;
    return true;
}

bool
sc_recording_clock_complete(struct sc_recording_clock *clock, uint32_t sequence,
                             int64_t device_us, int64_t host_receive_us) {
    struct sc_recording_clock_pending *pending = NULL;
    for (unsigned i = 0; i < SC_RECORDING_CLOCK_SLOTS; ++i) {
        if (clock->pending[i].valid && clock->pending[i].sequence == sequence) {
            pending = &clock->pending[i];
            break;
        }
    }
    if (!pending) return false;
    pending->valid = false;
    if (device_us < 0 || host_receive_us < pending->sent_us
            || (clock->calibrated && host_receive_us < clock->updated_us)) return false;
    int64_t rtt = host_receive_us - pending->sent_us;
    if (rtt > SC_RECORDING_CLOCK_MAX_RTT_US) return false;
    int64_t midpoint = pending->sent_us + rtt / 2;
    // Both operands are nonnegative, so this difference cannot overflow.
    int64_t offset = device_us - midpoint;
    int64_t difference;
    if (clock->calibrated && (!subtract(offset, clock->target_offset_us, &difference)
            || difference < -100000 || difference > 100000)) return false;

    clock->samples[clock->sample_next] = (struct sc_recording_clock_sample) {
        .valid = true, .received_us = host_receive_us,
        .rtt_us = rtt, .offset_us = offset,
    };
    clock->sample_next = (clock->sample_next + 1) % SC_RECORDING_CLOCK_SLOTS;
    const struct sc_recording_clock_sample *best = NULL;
    for (unsigned i = 0; i < SC_RECORDING_CLOCK_SLOTS; ++i) {
        struct sc_recording_clock_sample *sample = &clock->samples[i];
        if (!sample->valid) continue;
        if (host_receive_us - sample->received_us > SC_RECORDING_CLOCK_EXPIRY_US) {
            sample->valid = false;
            continue;
        }
        if (!best || sample->rtt_us < best->rtt_us
                || (sample->rtt_us == best->rtt_us
                    && sample->received_us > best->received_us)) best = sample;
    }
    // The newly accepted sample always provides at least one valid candidate.
    clock->target_offset_us = best->offset_us;
    clock->best_rtt_us = best->rtt_us;
    if (!clock->calibrated || !clock->mapped) {
        clock->offset_us = best->offset_us;
    }
    // Once capture has started, map() applies the new target gradually along
    // the sample timeline. A delayed reply must not step the next tiny chunk.
    clock->updated_us = host_receive_us;
    clock->calibrated = true;
    return true;
}

bool
sc_recording_clock_ready(const struct sc_recording_clock *clock, int64_t now_host_us) {
    return clock->calibrated && now_host_us >= clock->updated_us
        && now_host_us - clock->updated_us <= SC_RECORDING_CLOCK_EXPIRY_US;
}

bool
sc_recording_clock_map(struct sc_recording_clock *clock, int64_t host_pts,
                        int64_t now_host_us, int64_t *device_pts) {
    if (!device_pts || host_pts < 0 || !sc_recording_clock_ready(clock, now_host_us)
            || (clock->mapped && host_pts <= clock->last_host_pts)) return false;
    int64_t offset = clock->offset_us;
    unsigned remainder = clock->slew_remainder_us;
    if (clock->mapped) {
        int64_t difference;
        if (!subtract(clock->target_offset_us, offset, &difference)) return false;
        int64_t elapsed = host_pts - clock->last_host_pts;
        int64_t allowance = elapsed / 1000;
        remainder += elapsed % 1000;
        allowance += remainder / 1000;
        remainder %= 1000;
        // At most 1,000 us/second, accumulated even for sub-millisecond
        // callbacks. Keep at least one microsecond of forward progress.
        if (allowance >= elapsed) allowance = elapsed - 1;
        if (difference > allowance) difference = allowance;
        else if (difference < -allowance) difference = -allowance;
        offset += difference; // bounded between the current and target offsets
    }
    if ((offset > 0 && host_pts > INT64_MAX - offset)
            || (offset < 0 && host_pts < -offset)) return false;
    int64_t mapped = host_pts + offset;
    if (clock->mapped && mapped <= clock->last_device_pts) return false;
    clock->mapped = true;
    clock->offset_us = offset;
    clock->slew_remainder_us = remainder;
    clock->last_host_pts = host_pts;
    clock->last_device_pts = mapped;
    *device_pts = mapped;
    return true;
}

int64_t
sc_recording_clock_uncertainty_us(const struct sc_recording_clock *clock) {
    if (!clock->calibrated) return INT64_MAX;
    int64_t difference;
    if (!subtract(clock->target_offset_us, clock->offset_us, &difference)
            || difference == INT64_MIN) return INT64_MAX;
    if (difference < 0) difference = -difference;
    int64_t half_rtt = (clock->best_rtt_us + 1) / 2;
    return difference <= INT64_MAX - half_rtt ? difference + half_rtt : INT64_MAX;
}
