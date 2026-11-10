package com.genymobile.scrcpy.video;

import com.genymobile.scrcpy.util.Ln;

import android.media.MediaCodec;
import android.os.Bundle;

public class CaptureControl {

    public static final int RESET_REASON_TERMINATED = 1;
    public static final int RESET_REASON_DISPLAY_PROPERTIES_CHANGED = 1 << 1;
    public static final int RESET_REASON_CLIENT_RESET = 1 << 2;
    public static final int RESET_REASON_CLIENT_RESIZED = 1 << 3;

    private int reset = 0;

    // Current instance of MediaCodec to "interrupt" on reset
    private MediaCodec runningMediaCodec;
    private boolean keyFrameRequested;

    public synchronized boolean isResetRequested() {
        return reset != 0;
    }

    public synchronized int consumeReset() {
        int value = reset;
        reset = 0;
        return value;
    }

    public synchronized void reset(int reason) {
        assert reason != 0;
        reset |= reason;
        if (runningMediaCodec != null) {
            try {
                runningMediaCodec.signalEndOfInputStream();
            } catch (IllegalStateException e) {
                // ignore
            }
        }
    }

    public synchronized void setRunningMediaCodec(MediaCodec runningMediaCodec) {
        this.runningMediaCodec = runningMediaCodec;
        requestPendingKeyFrame();
    }

    /** Request a keyframe without restarting the encoder or display capture. */
    public synchronized void requestKeyFrame() {
        keyFrameRequested = true;
        requestPendingKeyFrame();
    }

    // Called with this monitor held. SurfaceEncoder publishes only a started
    // codec and clears it under the same monitor before stop/reset/release.
    private void requestPendingKeyFrame() {
        if (!keyFrameRequested || runningMediaCodec == null || reset != 0) {
            // Preserve requests made during startup or a real capture reset.
            return;
        }

        Bundle parameters = new Bundle();
        parameters.putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0);
        try {
            runningMediaCodec.setParameters(parameters);
            keyFrameRequested = false;
        } catch (IllegalStateException | IllegalArgumentException e) {
            // Keep the request for a replacement codec, but never reset the
            // display merely to start recording. The client has a startup timeout.
            Ln.w("Could not request video keyframe; recording may wait for the next keyframe", e);
        }
    }
}
