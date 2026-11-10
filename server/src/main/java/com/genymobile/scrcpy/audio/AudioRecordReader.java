package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.AndroidVersions;
import com.genymobile.scrcpy.util.Ln;

import android.annotation.TargetApi;
import android.media.AudioRecord;
import android.media.AudioTimestamp;
import android.media.MediaCodec;

import java.nio.ByteBuffer;

public class AudioRecordReader {

    private final AudioRecord recorder;

    private final AudioTimestamp timestamp = new AudioTimestamp();
    private final AudioTimestampTracker clock = new AudioTimestampTracker(AudioConfig.SAMPLE_RATE);
    private boolean warnedTimestamp;

    public AudioRecordReader(AudioRecord recorder) {
        this.recorder = recorder;
    }

    public boolean hasReliableTimestamps() {
        return clock.hasRecentHardwareTimestamp(System.nanoTime());
    }

    @TargetApi(AndroidVersions.API_24_ANDROID_7_0)
    public int read(ByteBuffer outDirectBuffer, MediaCodec.BufferInfo outBufferInfo) {
        int r = recorder.read(outDirectBuffer, AudioConfig.MAX_READ_SIZE);
        if (r <= 0) {
            return r;
        }

        int ret = recorder.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_MONOTONIC);
        long now = System.nanoTime();
        long pts = clock.read(r / (AudioConfig.CHANNELS * AudioConfig.BYTES_PER_SAMPLE), ret == AudioRecord.SUCCESS,
                timestamp.framePosition, timestamp.nanoTime, now);
        if (!clock.hasRecentHardwareTimestamp(now) && !warnedTimestamp) {
            Ln.w("Device audio timestamp unavailable; estimating from captured sample count");
            warnedTimestamp = true;
        }

        outBufferInfo.set(0, r, pts, 0);
        return r;
    }
}
