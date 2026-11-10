package com.genymobile.scrcpy.audio;

import com.genymobile.scrcpy.control.DeviceMessage;

/** One opt-in microphone worker. No Android resources exist while idle. */
public final class MicrophoneSession {
    public interface Capture extends AutoCloseable {
        Frame read() throws Exception;
        boolean isUnsilenced();
        @Override
        void close();
    }

    public interface Factory {
        Capture open() throws Exception;
    }

    public interface OutputState {
        boolean isUnsilenced();
    }

    public interface Clock {
        long nanoTime();
    }

    public interface Sender {
        void beginMicSession(int session);
        void endMicSession(int session);
        boolean sendMic(DeviceMessage message);
        void send(DeviceMessage message);
    }

    public static final class Frame {
        public final byte[] data;
        public final long pts;
        public final boolean timestampReliable;

        public Frame(byte[] data, long pts, boolean timestampReliable) {
            this.data = data;
            this.pts = pts;
            this.timestampReliable = timestampReliable;
        }
    }

    private static final long START_TIMEOUT_NS = 2000000000L;
    private static final long LEASE_TIMEOUT_NS = 5000000000L;
    private final Factory factory;
    private final OutputState output;
    private final Sender sender;
    private final Clock clock;
    private boolean stopped;
    private boolean requested;
    private int requestedSession;
    private int activeSession;
    private long leaseUpdated;
    private Thread thread;

    public MicrophoneSession(Factory factory, OutputState output, Sender sender) {
        this(factory, output, sender, System::nanoTime);
    }

    public MicrophoneSession(Factory factory, OutputState output, Sender sender, Clock clock) {
        this.factory = factory;
        this.output = output;
        this.sender = sender;
        this.clock = clock;
    }

    public synchronized void request(boolean enabled, int session) {
        if (stopped) return;
        if (enabled) {
            if (session == 0) throw new IllegalArgumentException("Invalid microphone session");
            leaseUpdated = clock.nanoTime();
            if (requested && requestedSession == session) return;
            sender.endMicSession(0);
            requestedSession = session;
            requested = true;
        } else if (session == 0 || session == requestedSession) {
            requested = false;
            sender.endMicSession(0);
            if (activeSession == 0) {
                sender.send(DeviceMessage.createMicStatus(session, DeviceMessage.MIC_STOPPED, "Android Mic off"));
            }
        }
        notifyAll();
    }

    // A clock probe renews only a live explicitly requested session. It cannot
    // open a microphone or revive one that has stopped/failed.
    public synchronized void heartbeat(int session) {
        if (!stopped && requested && requestedSession == session) leaseUpdated = clock.nanoTime();
    }

    private synchronized boolean leaseExpired(long now) {
        return now - leaseUpdated > LEASE_TIMEOUT_NS;
    }

    private synchronized boolean wanted(int session) {
        return !stopped && requested && requestedSession == session;
    }

    public synchronized void start() {
        if (thread != null) throw new IllegalStateException("Microphone worker already started");
        thread = new Thread(this::run, "android-mic");
        thread.start();
    }

    public synchronized void stop() {
        stopped = true;
        requested = false;
        sender.endMicSession(0);
        notifyAll();
        if (thread != null) thread.interrupt();
    }

    public void join() throws InterruptedException {
        Thread current;
        synchronized (this) { current = thread; }
        if (current != null) current.join();
    }

    private void run() {
        try {
            for (;;) {
                int session;
                synchronized (this) {
                    while (!requested && !stopped) wait();
                    if (stopped) return;
                    session = requestedSession;
                    activeSession = session;
                }
                capture(session);
            }
        } catch (InterruptedException e) {
            // The capture scope releases the recorder before worker shutdown.
            Thread.currentThread().interrupt();
        }
    }

    private void capture(int session) throws InterruptedException {
        String error = null;
        boolean ready = false;
        long started = clock.nanoTime();
        long lastFrame = started;
        try {
            // Output must be active before opening the microphone. Do not
            // silently replace device audio or create a mic-only recording.
            if (leaseExpired(started)) {
                error = "Android Mic connection heartbeat expired.";
            } else if (!output.isUnsilenced()) {
                error = "Android device audio or its capture timestamps are unavailable; microphone was not started.";
            } else if (wanted(session)) {
                try (Capture capture = factory.open()) {
                    if (wanted(session)) sender.beginMicSession(session);
                    while (wanted(session)) {
                        long now = clock.nanoTime();
                        if (leaseExpired(now)) {
                            error = "Android Mic connection heartbeat expired.";
                            break;
                        }
                        if (!output.isUnsilenced()) {
                            error = "Android device audio was silenced or lost its timestamps; synchronized microphone recording stopped.";
                            break;
                        }
                        boolean unsilenced = capture.isUnsilenced();
                        if (ready && !unsilenced) {
                            error = "Android microphone was silenced or access was revoked.";
                            break;
                        }
                        Frame frame = capture.read();
                        if (frame != null) {
                            lastFrame = now;
                            if (unsilenced && frame.timestampReliable) {
                                synchronized (this) {
                                    if (!wanted(session)) break;
                                    if (!ready) {
                                        sender.send(DeviceMessage.createMicStatus(session, DeviceMessage.MIC_READY, "Android Mic recording"));
                                        ready = true;
                                    }
                                    if (!sender.sendMic(DeviceMessage.createMicPcm(session, frame.pts, frame.data))) {
                                        error = "Android microphone transport is too slow; recording cannot retain every source.";
                                        break;
                                    }
                                }
                            } else if (ready) {
                                error = "Android microphone timestamps became unavailable; synchronized recording stopped.";
                                break;
                            }
                        }
                        if (!ready && now - started > START_TIMEOUT_NS) {
                            error = "Android microphone is unsupported, silenced, or has no reliable capture timestamps.";
                            break;
                        }
                        if (now - lastFrame > START_TIMEOUT_NS) {
                            error = "Android microphone stopped delivering audio.";
                            break;
                        }
                        if (frame == null) Thread.sleep(5);
                    }
                }
            }
        } catch (SecurityException e) {
            error = "Android denied microphone access. Check the phone microphone privacy setting and recording permissions.";
        } catch (InterruptedException e) {
            throw e;
        } catch (Exception e) {
            error = "Android microphone capture is unavailable on this device.";
        } finally {
            synchronized (this) {
                sender.endMicSession(session);
                // A newer request owns its own state. Never cancel it or turn
                // its microphone indicator off with an old-session result.
                boolean current = requestedSession == session;
                boolean failed = error != null && wanted(session);
                if (current) requested = false;
                activeSession = 0;
                sender.send(DeviceMessage.createMicStatus(session,
                        failed ? DeviceMessage.MIC_ERROR : DeviceMessage.MIC_STOPPED,
                        failed ? error : "Android Mic off"));
            }
        }
    }
}
