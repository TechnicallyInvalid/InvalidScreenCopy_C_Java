package com.genymobile.scrcpy.control;

import com.genymobile.scrcpy.util.Ln;
import com.genymobile.scrcpy.audio.MicrophoneSession;

import java.io.IOException;
import java.util.ArrayDeque;
import java.util.Iterator;

public final class DeviceMessageSender implements MicrophoneSession.Sender {

    private static final int QUEUE_CAPACITY = 16;
    private final ControlChannel controlChannel;
    private Thread thread;
    private final ArrayDeque<DeviceMessage> queue = new ArrayDeque<>();
    private final ArrayDeque<DeviceMessage> priorityQueue = new ArrayDeque<>();
    private final ArrayDeque<DeviceMessage> micQueue = new ArrayDeque<>();
    private int micSession;

    public DeviceMessageSender(ControlChannel controlChannel) {
        this.controlChannel = controlChannel;
    }

    public synchronized void send(DeviceMessage msg) {
        int type = msg.getType();
        if (type == DeviceMessage.TYPE_MIC_STATUS || type == DeviceMessage.TYPE_AUDIO_CLOCK) {
            // A terminal state replaces older state for the same session. Clock
            // probes/status must not wait behind PCM or clipboard payloads.
            if (type == DeviceMessage.TYPE_MIC_STATUS) {
                for (DeviceMessage old : priorityQueue) {
                    if (old.getType() == type && old.getSession() == msg.getSession()
                            && old.getStatus() == DeviceMessage.MIC_ERROR && msg.getStatus() != DeviceMessage.MIC_ERROR) {
                        // A subsequent Stop must not hide a recording failure
                        // that the desktop has not received yet.
                        return;
                    }
                }
                priorityQueue.removeIf(old -> old.getType() == type && old.getSession() == msg.getSession());
            }
            if (priorityQueue.size() == QUEUE_CAPACITY) {
                Iterator<DeviceMessage> iterator = priorityQueue.iterator();
                while (iterator.hasNext()) {
                    if (iterator.next().getType() == DeviceMessage.TYPE_AUDIO_CLOCK) {
                        iterator.remove();
                        break;
                    }
                }
                if (priorityQueue.size() == QUEUE_CAPACITY) priorityQueue.removeFirst();
            }
            priorityQueue.addLast(msg);
            notifyAll();
            return;
        }
        if (queue.size() == QUEUE_CAPACITY) {
            Ln.w("Device message dropped: " + type);
            return;
        }
        queue.addLast(msg);
        notifyAll();
    }

    public synchronized void beginMicSession(int session) {
        micQueue.clear();
        micSession = session;
    }

    public synchronized void endMicSession(int session) {
        if (session == 0 || micSession == session) {
            micSession = 0;
            micQueue.clear();
        }
    }

    // PCM uses its own bounded queue. Never block capture or silently omit a
    // chosen source: false makes the capture session report a terminal error.
    public synchronized boolean sendMic(DeviceMessage msg) {
        if (msg.getType() != DeviceMessage.TYPE_MIC_PCM || msg.getSession() != micSession || micSession == 0
                || micQueue.size() == QUEUE_CAPACITY) {
            return false;
        }
        micQueue.addLast(msg);
        notifyAll();
        return true;
    }

    private void loop() throws IOException, InterruptedException {
        while (!Thread.currentThread().isInterrupted()) {
            DeviceMessage msg;
            synchronized (this) {
                while (priorityQueue.isEmpty() && queue.isEmpty() && micQueue.isEmpty()) wait();
                msg = !priorityQueue.isEmpty() ? priorityQueue.removeFirst()
                        : !queue.isEmpty() ? queue.removeFirst() : micQueue.removeFirst();
            }
            controlChannel.send(msg);
        }
    }

    public void start() {
        thread = new Thread(() -> {
            try {
                loop();
            } catch (IOException | InterruptedException e) {
                // This is expected on close. The receiver/recording deadline
                // observes disconnect if no terminal microphone status arrives.
            } finally {
                Ln.d("Device message sender stopped");
            }
        }, "control-send");
        thread.start();
    }

    public void stop() {
        endMicSession(0);
        if (thread != null) thread.interrupt();
    }

    public void join() throws InterruptedException {
        if (thread != null) thread.join();
    }
}
