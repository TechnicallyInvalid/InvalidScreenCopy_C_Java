package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.AndroidVersions;
import com.genymobile.scrcpy.FakeContext;
import com.genymobile.scrcpy.control.DeviceMessage;
import com.genymobile.scrcpy.control.DeviceMessageSender;

import android.annotation.SuppressLint;
import android.annotation.TargetApi;
import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.media.AudioRecordingConfiguration;
import android.media.AudioTimestamp;
import android.media.MediaRecorder;
import android.os.Build;
import android.os.SystemClock;

import java.nio.ByteBuffer;

/** Android adapter for the tested, opt-in microphone session lifecycle. */
public final class AndroidMicCapture {
    private volatile AudioCapture output;
    private final MicrophoneSession session;

    public AndroidMicCapture(DeviceMessageSender sender) {
        session = new MicrophoneSession(Recorder::new, () -> {
            AudioCapture current = output;
            return current != null && current.isCapturingWithoutSilencing() && current.hasReliableTimestamps();
        }, sender, SystemClock::elapsedRealtimeNanos);
    }

    public void setAudioCapture(AudioCapture output) { this.output = output; }
    public void request(boolean enabled, int id) { session.request(enabled, id); }
    public void heartbeat(int id) { session.heartbeat(id); }
    public void start() { session.start(); }
    public void stop() { session.stop(); }
    public void join() throws InterruptedException { session.join(); }

    @TargetApi(AndroidVersions.API_31_ANDROID_12)
    private static final class Recorder implements MicrophoneSession.Capture {
        private final AudioRecord recorder;
        private final AudioManager audioManager;
        private final ByteBuffer buffer = ByteBuffer.allocateDirect(DeviceMessage.MIC_MAX_BYTES);
        private final AudioTimestamp timestamp = new AudioTimestamp();
        private final AudioTimestampTracker clock = new AudioTimestampTracker(48000);

        @SuppressLint("MissingPermission")
        Recorder() {
            // Do not launch shell activities or alter permissions to make mic
            // access work. Unsupported/denied cases are reported to the user.
            if (Build.VERSION.SDK_INT < AndroidVersions.API_31_ANDROID_12) {
                throw new UnsupportedOperationException("Android Mic requires Android 12 or newer");
            }
            audioManager = (AudioManager) FakeContext.get().getSystemService(Context.AUDIO_SERVICE);
            if (audioManager == null) throw new UnsupportedOperationException("Microphone mute state unavailable");
            if (audioManager.isMicrophoneMute()) throw new SecurityException("Android microphone is muted");
            AudioFormat format = new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_IN_MONO).build();
            int minimum = AudioRecord.getMinBufferSize(48000, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (minimum <= 0) throw new UnsupportedOperationException("Mono microphone format unavailable");
            AudioRecord created = new AudioRecord.Builder().setContext(FakeContext.get())
                    .setAudioSource(MediaRecorder.AudioSource.MIC).setAudioFormat(format)
                    .setPrivacySensitive(false).setBufferSizeInBytes(Math.max(minimum * 4, DeviceMessage.MIC_MAX_BYTES * 4)).build();
            try {
                if (created.getState() != AudioRecord.STATE_INITIALIZED) {
                    throw new UnsupportedOperationException("Microphone could not initialize");
                }
                if (created.getSampleRate() != 48000 || created.getChannelCount() != 1
                        || created.getAudioFormat() != AudioFormat.ENCODING_PCM_16BIT) {
                    throw new UnsupportedOperationException("Microphone did not honor the requested PCM format");
                }
                created.startRecording();
                if (created.getRecordingState() != AudioRecord.RECORDSTATE_RECORDING) {
                    throw new SecurityException("Microphone did not start recording");
                }
                recorder = created;
            } catch (RuntimeException e) {
                created.release();
                throw e;
            }
        }

        @Override
        public MicrophoneSession.Frame read() {
            buffer.clear();
            int count = recorder.read(buffer, buffer.capacity(), AudioRecord.READ_NON_BLOCKING);
            if (count == 0) return null;
            if (count < 0 || (count & 1) != 0) throw new IllegalStateException("Microphone read failed");
            long now = System.nanoTime();
            boolean available = recorder.getTimestamp(timestamp, AudioTimestamp.TIMEBASE_MONOTONIC) == AudioRecord.SUCCESS;
            long pts = clock.read(count / 2, available, timestamp.framePosition, timestamp.nanoTime, now);
            byte[] data = new byte[count];
            buffer.position(0);
            buffer.get(data);
            return new MicrophoneSession.Frame(data, pts, clock.hasRecentHardwareTimestamp(now));
        }

        @Override
        public boolean isUnsilenced() {
            AudioRecordingConfiguration configuration = recorder.getActiveRecordingConfiguration();
            return recorder.getRecordingState() == AudioRecord.RECORDSTATE_RECORDING
                    && configuration != null && !configuration.isClientSilenced() && !audioManager.isMicrophoneMute();
        }

        @Override
        public void close() {
            // release() also stops capture. It runs before STOPPED/ERROR is sent.
            recorder.release();
        }
    }
}
