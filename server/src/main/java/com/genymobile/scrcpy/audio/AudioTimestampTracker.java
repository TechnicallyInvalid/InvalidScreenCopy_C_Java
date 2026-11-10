package com.genymobile.scrcpy.audio;

/** Maps a capture read's first sample onto Android's monotonic clock. */
public final class AudioTimestampTracker {
    private final int sampleRate;
    private long framesRead;
    private long nextPts;
    private long previousPts = -1;
    private long lastHardwareTimestamp = -1;
    private volatile long lastHardwareObservation = -1;
    private volatile boolean discontinuity;

    public AudioTimestampTracker(int sampleRate) {
        if (sampleRate <= 0) throw new IllegalArgumentException("Invalid sample rate");
        this.sampleRate = sampleRate;
    }

    public long read(int frames, boolean timestampAvailable, long framePosition, long timestampNanos, long nowNanos) {
        if (frames <= 0) throw new IllegalArgumentException("Empty capture read");
        long duration = frames * 1000000L / sampleRate;
        long pts = nextPts != 0 ? nextPts : Math.max(0, nowNanos / 1000 - duration);
        if (timestampAvailable && framePosition >= 0 && timestampNanos > 0) {
            // AudioTimestamp describes framePosition, not the start of the
            // buffer just read. Account for any samples queued in AudioRecord.
            long candidate = timestampNanos / 1000 + (framesRead - framePosition) * 1000000L / sampleRate;
            if (candidate >= 0 && Math.abs(candidate - nowNanos / 1000) <= 2000000) {
                // A large jump after acquiring a hardware timeline can indicate
                // dropped capture samples or a broken vendor clock. Keep the
                // legacy output monotonic, but do not certify mic synchronization.
                if (lastHardwareObservation >= 0 && (candidate < nextPts - 50000 || candidate > nextPts + 250000)) {
                    discontinuity = true;
                }
                if (!discontinuity) pts = candidate;
                if (!discontinuity && timestampNanos != lastHardwareTimestamp) {
                    lastHardwareTimestamp = timestampNanos;
                    lastHardwareObservation = nowNanos;
                }
            }
        }
        if (pts <= previousPts) pts = previousPts + 1;
        framesRead += frames;
        previousPts = pts;
        nextPts = pts + duration;
        return pts;
    }

    public boolean hasRecentHardwareTimestamp(long nowNanos) {
        return !discontinuity && lastHardwareObservation >= 0 && nowNanos >= lastHardwareObservation
                && nowNanos - lastHardwareObservation <= 2000000000L;
    }
}
