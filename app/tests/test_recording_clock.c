#include "recording_clock.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

static int64_t
device_time(int64_t host, int64_t offset, int ppm) {
    return host + offset + host * ppm / 1000000;
}

static bool
sample(struct sc_recording_clock *clock, uint32_t sequence, int64_t host,
        int64_t forward, int64_t reverse, int64_t offset, int ppm) {
    assert(sc_recording_clock_begin(clock, sequence, host));
    return sc_recording_clock_complete(clock, sequence,
        device_time(host + forward, offset, ppm), host + forward + reverse);
}

static void
test_midpoint_and_slew(void) {
    struct sc_recording_clock clock;
    sc_recording_clock_reset(&clock);
    assert(!sc_recording_clock_ready(&clock, 0));
    assert(sc_recording_clock_uncertainty_us(&clock) == INT64_MAX);
    const int64_t offset = 5000000;
    assert(sample(&clock, 1, 1000000, 40000, 10000, offset, 0));
    // With asymmetric latency the exact offset is unknowable, but the
    // midpoint error is at most half the measured round-trip time.
    assert(llabs(clock.offset_us - offset) <= sc_recording_clock_uncertainty_us(&clock));
    assert(clock.offset_us == offset + 15000);
    assert(sample(&clock, 2, 1100000, 1000, 1000, offset, 0));
    assert(clock.offset_us == offset); // immediate improvement before capture
    int64_t result;
    assert(sc_recording_clock_map(&clock, 1200000, 1200000, &result));
    assert(result == 1200000 + offset);
    assert(!sc_recording_clock_map(&clock, 1200000, 1200001, &result));
    assert(!sc_recording_clock_map(&clock, 1199999, 1200001, &result));
    int64_t old_offset = clock.offset_us;
    assert(sample(&clock, 3, 3100000, 500, 500, offset + 5000, 0));
    assert(clock.offset_us == old_offset); // clock reply never steps capture PTS
    assert(sc_recording_clock_uncertainty_us(&clock) >= 5500);
    assert(sc_recording_clock_map(&clock, 3120000, 3120000, &result));
    assert(result > 1200000 + offset);
    assert(clock.offset_us - old_offset <= 1920);
}

static void
test_rejections_and_expiry(void) {
    struct sc_recording_clock clock;
    sc_recording_clock_reset(&clock);
    assert(!sc_recording_clock_begin(&clock, 0, -1));
    assert(sc_recording_clock_begin(&clock, 1, 1000000));
    assert(!sc_recording_clock_begin(&clock, 1, 1000001));
    assert(!sc_recording_clock_complete(&clock, 2, 2000000, 1001000));
    assert(!sc_recording_clock_complete(&clock, 1, 2000000, 999999));
    assert(!sc_recording_clock_complete(&clock, 1, 2000000, 1001000));
    assert(!sample(&clock, 3, 1100000, 25000, 25001, 5000000, 0));
    assert(!clock.calibrated);
    assert(sample(&clock, 4, 1200000, 500, 500, 5000000, 0));
    int64_t accepted = clock.updated_us;
    assert(!sample(&clock, 5, 1300000, 500, 500, 5200000, 0));
    assert(clock.updated_us == accepted);
    assert(sc_recording_clock_ready(&clock, accepted + SC_RECORDING_CLOCK_EXPIRY_US));
    assert(!sc_recording_clock_ready(&clock, accepted + SC_RECORDING_CLOCK_EXPIRY_US + 1));
    int64_t result = 7;
    assert(!sc_recording_clock_map(&clock, 1500000,
        accepted + SC_RECORDING_CLOCK_EXPIRY_US + 1, &result));
    assert(result == 7);
    // Old queued replies cannot consume a newly assigned slot or change offset.
    for (unsigned i = 0; i < SC_RECORDING_CLOCK_SLOTS + 1; ++i)
        assert(sc_recording_clock_begin(&clock, 100 + i, 1400000 + i));
    assert(!sc_recording_clock_complete(&clock, 100, 6400500, 1401000));
    assert(!sc_recording_clock_complete(&clock, 108, -1, 1401000));
    assert(clock.updated_us == accepted);
}

static void
test_drift(int ppm) {
    struct sc_recording_clock clock;
    sc_recording_clock_reset(&clock);
    const int64_t offset = 5000000;
    uint32_t sequence = 1;
    for (int i = 0; i < 5; ++i) {
        assert(sample(&clock, sequence++, 1000000 + i * 100000,
                       600, 400, offset, ppm));
    }
    int64_t previous = -1;
    // Six synthetic minutes of 20-ms PCM chunks. Clock samples refresh every
    // two seconds with unequal transport delays and both faster/slower clocks.
    for (int frame = 0; frame <= 18000; ++frame) {
        int64_t host = 2000000 + (int64_t) frame * 20000;
        if (!(frame % 100)) {
            unsigned n = sequence++;
            assert(sample(&clock, n, host, 600 + n % 4 * 100,
                           400 + n % 3 * 100, offset, ppm));
        }
        int64_t mapped;
        assert(sc_recording_clock_map(&clock, host, host + 6000, &mapped));
        assert(mapped > previous);
        assert(llabs(mapped - device_time(host, offset, ppm)) < 4000);
        previous = mapped;
    }
}

static void
test_integer_boundaries(void) {
    struct sc_recording_clock clock;
    sc_recording_clock_reset(&clock);
    assert(sc_recording_clock_begin(&clock, 1, 0));
    assert(sc_recording_clock_complete(&clock, 1, INT64_MAX, 0));
    int64_t result = 0;
    assert(sc_recording_clock_map(&clock, 0, 0, &result));
    assert(result == INT64_MAX);
    assert(!sc_recording_clock_map(&clock, 1, 1, &result));
    sc_recording_clock_reset(&clock);
    assert(sc_recording_clock_begin(&clock, 1, INT64_MAX - 100));
    assert(sc_recording_clock_complete(&clock, 1, 0, INT64_MAX));
    assert(!sc_recording_clock_map(&clock, 0, INT64_MAX, &result));
    assert(sc_recording_clock_map(&clock, INT64_MAX, INT64_MAX, &result));
    assert(result == 50);
    assert(!sc_recording_clock_ready(&clock, -1));
}

static void
test_tiny_chunks(unsigned frames) {
    struct sc_recording_clock clock;
    sc_recording_clock_reset(&clock);
    const int64_t offset = 5000000;
    assert(sample(&clock, 1, 1000000, 500, 500, offset, 0));
    unsigned sequence = 2;
    int64_t next_query = 2000000;
    int64_t previous;
    int64_t previous_host = 1980000;
    assert(sc_recording_clock_map(&clock, previous_host, previous_host, &previous));
    // 32/64-sample SDL chunks are shorter than a reply-time 2-ms slew step.
    // Exercise negative correction on every two-second query for ten seconds.
    for (unsigned chunk = 0; chunk < 480000 / frames; ++chunk) {
        int64_t host = 2000000 + (int64_t) chunk * frames * 1000000 / 48000;
        if (host >= next_query) {
            int64_t before = clock.offset_us;
            assert(sample(&clock, sequence++, host, 500, 500,
                           offset - 20000, 0));
            assert(clock.offset_us == before);
            next_query = host + 2000000;
        }
        int64_t mapped;
        assert(sc_recording_clock_map(&clock, host, host + 2000, &mapped));
        if (previous >= 0) {
            int64_t advance = host - previous_host;
            assert(mapped > previous);
            // Fractional accumulated slew may contribute one extra us.
            assert(mapped - previous >= advance - advance / 1000 - 1);
            assert(mapped - previous <= advance);
        }
        previous = mapped;
        previous_host = host;
    }
    // The older equal-RTT sample is superseded by the latest accepted one;
    // correction proceeds smoothly, without exceeding 0.1% or stalling at 0
    // merely because each small callback has less than one millisecond.
    assert(clock.offset_us < offset - 9000);
    assert(clock.offset_us > offset - 11000);
}

int
main(void) {
    test_midpoint_and_slew();
    test_rejections_and_expiry();
    test_drift(100);
    test_drift(200);
    test_drift(-200);
    test_integer_boundaries();
    test_tiny_chunks(32);
    test_tiny_chunks(64);
    return 0;
}
