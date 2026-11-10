#!/usr/bin/env python3
"""Exercise production Java mic lifecycle/clock/queues with fake capture only.

Uses the host JDK and deterministic capture gates/clock injection. Android audio,
permissions, connected devices, GUI and network APIs are never exercised.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


JAVA = r"""
import com.genymobile.scrcpy.audio.AudioTimestampTracker;
import com.genymobile.scrcpy.audio.MicrophoneSession;
import com.genymobile.scrcpy.control.ControlChannel;
import com.genymobile.scrcpy.control.DeviceMessage;
import com.genymobile.scrcpy.control.DeviceMessageSender;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

public class AndroidMicTest {
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    static void await(CountDownLatch latch) throws Exception {
        check(latch.await(2, TimeUnit.SECONDS), "Capture gate timed out");
    }
    static final MicrophoneSession.Frame EMPTY = new MicrophoneSession.Frame(new byte[0], 0, false);
    static MicrophoneSession.Frame frame(boolean reliable) {
        return new MicrophoneSession.Frame(new byte[]{1, 0, 2, 0}, 10000000, reliable);
    }
    static final class FakeClock implements MicrophoneSession.Clock {
        final AtomicLong now = new AtomicLong(10000000000L);
        public long nanoTime() { return now.get(); }
        void advance(long millis) { now.addAndGet(millis * 1000000); }
    }
    static final class Capture implements MicrophoneSession.Capture {
        final LinkedBlockingQueue<MicrophoneSession.Frame> input = new LinkedBlockingQueue<>();
        final CountDownLatch entered = new CountDownLatch(1);
        final CountDownLatch closed = new CountDownLatch(1);
        volatile boolean unsilenced = true;
        volatile int closes;
        public MicrophoneSession.Frame read() throws InterruptedException {
            entered.countDown();
            MicrophoneSession.Frame result = input.take();
            return result == EMPTY ? null : result;
        }
        public boolean isUnsilenced() { return unsilenced; }
        public void close() { closes++; closed.countDown(); }
        void push(boolean reliable) { input.add(frame(reliable)); }
        void wake() { input.add(EMPTY); }
    }
    static final class Sender implements MicrophoneSession.Sender {
        final LinkedBlockingQueue<DeviceMessage> events = new LinkedBlockingQueue<>();
        volatile int active;
        volatile boolean rejectPcm;
        public void beginMicSession(int session) { active = session; }
        public void endMicSession(int session) {
            if (session == 0 || active == session) active = 0;
        }
        public boolean sendMic(DeviceMessage message) {
            check(message.getSession() == active && active != 0, "PCM sent outside active session");
            if (rejectPcm) return false;
            events.add(message);
            return true;
        }
        public void send(DeviceMessage message) { events.add(message); }
        DeviceMessage event(int type, int session, int status) throws Exception {
            DeviceMessage message = events.poll(2, TimeUnit.SECONDS);
            check(message != null, "Missing event " + type + "/" + session + "/" + status);
            check(message.getType() == type && message.getSession() == session,
                    "Wrong event type/session " + message.getType() + "/" + message.getSession());
            if (type == DeviceMessage.TYPE_MIC_STATUS)
                check(message.getStatus() == status, "Unexpected status " + message.getStatus());
            return message;
        }
        void ready(int session) throws Exception {
            event(DeviceMessage.TYPE_MIC_STATUS, session, DeviceMessage.MIC_READY);
            event(DeviceMessage.TYPE_MIC_PCM, session, 0);
        }
        DeviceMessage terminal(Capture capture, int session, int status) throws Exception {
            DeviceMessage message = event(DeviceMessage.TYPE_MIC_STATUS, session, status);
            check(capture.closed.getCount() == 0 && capture.closes == 1,
                    "Terminal status sent before exactly-once release");
            return message;
        }
    }
    static final class Fixture implements AutoCloseable {
        final FakeClock clock = new FakeClock();
        final Sender sender = new Sender();
        final List<Capture> captures = new ArrayList<>();
        volatile boolean output = true;
        volatile boolean deny;
        volatile boolean unsupported;
        volatile boolean blockOpen;
        final CountDownLatch openEntered = new CountDownLatch(1);
        final CountDownLatch openRelease = new CountDownLatch(1);
        volatile int opens;
        final MicrophoneSession session;
        Fixture(boolean start) {
            session = new MicrophoneSession(() -> {
                synchronized (captures) {
                    opens++;
                    if (deny) throw new SecurityException("synthetic denied");
                    if (unsupported) throw new UnsupportedOperationException("synthetic unsupported");
                    if (blockOpen) { openEntered.countDown(); await(openRelease); }
                    Capture capture = new Capture();
                    captures.add(capture);
                    captures.notifyAll();
                    return capture;
                }
            }, () -> output, sender, clock);
            if (start) session.start();
        }
        Capture capture(int index) throws Exception {
            long end = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
            synchronized (captures) {
                while (captures.size() <= index) {
                    long left = end - System.nanoTime();
                    check(left > 0, "Factory did not open expected capture");
                    TimeUnit.NANOSECONDS.timedWait(captures, left);
                }
                return captures.get(index);
            }
        }
        public void close() throws Exception {
            session.stop();
            session.join();
            for (Capture capture : captures) check(capture.closes == 1, "Leaked or double-closed capture");
        }
    }

    static void idleAndPendingLease() throws Exception {
        try (Fixture f = new Fixture(true)) {
            f.session.heartbeat(1);
            f.session.request(false, 0);
            f.sender.event(DeviceMessage.TYPE_MIC_STATUS, 0, DeviceMessage.MIC_STOPPED);
            check(f.opens == 0, "Off or a heartbeat opened microphone");
        }
        try (Fixture f = new Fixture(false)) {
            f.session.request(true, 8);
            f.clock.advance(6000);
            f.session.start();
            DeviceMessage error = f.sender.event(DeviceMessage.TYPE_MIC_STATUS, 8, DeviceMessage.MIC_ERROR);
            check(error.getText().contains("heartbeat expired"), "Wrong pending-expiry error");
            check(f.opens == 0, "Expired pending request opened microphone");
        }
    }

    static void readinessAndStop() throws Exception {
        try (Fixture f = new Fixture(true)) {
            f.blockOpen = true;
            f.session.request(true, 7);
            await(f.openEntered);
            f.session.request(false, 7);
            f.openRelease.countDown();
            Capture pending = f.capture(0);
            f.sender.terminal(pending, 7, DeviceMessage.MIC_STOPPED);
            check(pending.entered.getCount() == 1 && f.sender.events.isEmpty(),
                    "Cancelled opening capture read or sent PCM");
        }
        try (Fixture f = new Fixture(true)) {
            f.session.request(true, 1);
            Capture c = f.capture(0);
            await(c.entered);
            c.push(false);
            // An unreliable frame cannot produce Ready or PCM. A following
            // reliable frame yields the first status and frame in this queue.
            c.push(true);
            f.sender.ready(1);
            f.session.request(false, 1);
            c.push(true); // Must not escape after explicit disable.
            f.sender.terminal(c, 1, DeviceMessage.MIC_STOPPED);
            check(f.sender.events.isEmpty(), "PCM escaped after disable");
        }
        try (Fixture f = new Fixture(true)) {
            f.session.request(true, 2);
            Capture c = f.capture(0);
            await(c.entered);
            c.unsilenced = false;
            c.push(false);
            f.clock.advance(2100);
            c.push(false);
            f.sender.terminal(c, 2, DeviceMessage.MIC_ERROR);
        }
    }

    static void failures() throws Exception {
        try (Fixture f = new Fixture(true)) {
            f.output = false;
            f.session.request(true, 10);
            f.sender.event(DeviceMessage.TYPE_MIC_STATUS, 10, DeviceMessage.MIC_ERROR);
            check(f.opens == 0, "Unavailable output still opened mic");
        }
        for (boolean permission : new boolean[]{true, false}) {
            try (Fixture f = new Fixture(true)) {
                f.deny = permission;
                f.unsupported = !permission;
                f.session.request(true, 11);
                f.sender.event(DeviceMessage.TYPE_MIC_STATUS, 11, DeviceMessage.MIC_ERROR);
                check(f.opens == 1 && f.captures.isEmpty(), "Open failure leaked capture");
            }
        }
        for (int mode = 0; mode < 4; ++mode) {
            try (Fixture f = new Fixture(true)) {
                f.session.request(true, 12);
                Capture c = f.capture(0);
                c.push(true);
                f.sender.ready(12);
                if (mode == 0) { f.output = false; c.wake(); }
                else if (mode == 1) { c.unsilenced = false; c.wake(); }
                else if (mode == 2) { c.push(false); }
                else { f.sender.rejectPcm = true; c.push(true); }
                f.sender.terminal(c, 12, DeviceMessage.MIC_ERROR);
                f.session.heartbeat(12);
                check(f.opens == 1, "Heartbeat revived failed microphone");
            }
        }
    }

    static void leasesAndGenerations() throws Exception {
        try (Fixture f = new Fixture(true)) {
            f.session.request(true, 20);
            Capture c = f.capture(0);
            c.push(true);
            f.sender.ready(20);
            f.clock.advance(4000);
            f.session.heartbeat(20);
            f.clock.advance(4000);
            f.session.request(true, 20); // Same session renews without reopening.
            c.push(true);
            f.sender.event(DeviceMessage.TYPE_MIC_PCM, 20, 0);
            // Feed once more at the renewed clock so this checks lease expiry
            // independently of the separate two-second delivery-stall limit.
            c.push(true);
            f.sender.event(DeviceMessage.TYPE_MIC_PCM, 20, 0);
            check(f.opens == 1, "Renewal restarted capture");
            f.clock.advance(4000);
            f.session.heartbeat(999); // Wrong session cannot extend lease.
            f.clock.advance(2000);
            c.wake();
            DeviceMessage error = f.sender.terminal(c, 20, DeviceMessage.MIC_ERROR);
            check(error.getText().contains("heartbeat expired"), "Wrong active-expiry error");
        }
        try (Fixture f = new Fixture(true)) {
            f.session.request(true, 30);
            Capture old = f.capture(0);
            old.push(true);
            f.sender.ready(30);
            f.session.request(true, 31);
            old.push(true); // Generation check suppresses this stale frame.
            f.sender.terminal(old, 30, DeviceMessage.MIC_STOPPED);
            Capture current = f.capture(1);
            f.session.request(false, 30); // Stale stop must not stop session31.
            current.push(true);
            f.sender.ready(31);
            check(current.closes == 0 && f.opens == 2, "Stale stop closed new capture");
            f.session.request(false, 31);
            current.wake();
            f.sender.terminal(current, 31, DeviceMessage.MIC_STOPPED);
        }
        try (Fixture f = new Fixture(true)) {
            f.session.request(true, 40);
            Capture c = f.capture(0);
            c.push(true);
            f.sender.ready(40);
            f.clock.advance(2100);
            c.wake();
            // The worker may have sampled its clock before entering the
            // gated read. Let that read return, then return empty once more.
            c.wake();
            f.sender.terminal(c, 40, DeviceMessage.MIC_ERROR);
        }
    }

    static void timestamps() {
        AudioTimestampTracker tracker = new AudioTimestampTracker(48000);
        // Hardware describes frame960 at10.020s, but this read starts atframe0.
        check(tracker.read(480, true, 960, 10020000000L, 10021000000L) == 10000000,
                "Ignored framePosition/queued capture latency");
        check(tracker.hasRecentHardwareTimestamp(10021000000L), "Valid hardware timestamp unreliable");
        check(tracker.read(480, false, 0, 0, 10031000000L) == 10010000,
                "Fallback did not preserve sample cadence");
        check(!tracker.hasRecentHardwareTimestamp(12022000000L), "Stale hardware timestamp accepted");
        AudioTimestampTracker fallback = new AudioTimestampTracker(48000);
        check(fallback.read(480, false, 0, 0, 1000000000L) == 990000,
                "Initial fallback did not subtract read duration");
        check(fallback.read(480, false, 0, 0, 1000000000L) == 1000000,
                "Fallback timeline regressed");
        check(!fallback.hasRecentHardwareTimestamp(1000000000L), "Fallback certified as hardware time");
        tracker.read(480, true, 960, 11000000000L, 11001000000L);
        check(!tracker.hasRecentHardwareTimestamp(11001000000L), "Large established clock jump remained reliable");
        tracker.read(480, true, 1440, 11010000000L, 11011000000L);
        check(!tracker.hasRecentHardwareTimestamp(11011000000L), "Discontinuous clock silently recovered");
    }

    static void senderQueues() throws Exception {
        ControlChannel channel = new ControlChannel();
        DeviceMessageSender sender = new DeviceMessageSender(channel);
        DeviceMessage pcm = DeviceMessage.createMicPcm(1, 1, new byte[]{0, 0});
        check(!sender.sendMic(pcm), "Off sender accepted PCM");
        sender.beginMicSession(1);
        for (int i = 0; i < 16; ++i) check(sender.sendMic(pcm), "PCM queue rejected within bound");
        check(!sender.sendMic(pcm), "PCM queue exceeded sixteen packets");
        sender.send(DeviceMessage.createClipboard("ordinary"));
        sender.send(DeviceMessage.createMicStatus(1, DeviceMessage.MIC_ERROR, "synthetic error"));
        sender.send(DeviceMessage.createMicStatus(1, DeviceMessage.MIC_STOPPED, "off"));
        sender.start();
        DeviceMessage first = channel.messages.poll(2, TimeUnit.SECONDS);
        check(first != null && first.getStatus() == DeviceMessage.MIC_ERROR
                && first.getType() == DeviceMessage.TYPE_MIC_STATUS,
                "Stopped hid error or priority waited behind PCM/clipboard");
        DeviceMessage second = channel.messages.poll(2, TimeUnit.SECONDS);
        check(second != null && second.getType() == DeviceMessage.TYPE_CLIPBOARD,
                "Ordinary message starved behind PCM");
        sender.endMicSession(1);
        check(!sender.sendMic(pcm), "Ended sender accepted PCM");
        sender.stop(); sender.join();
    }

    public static void main(String[] args) throws Exception {
        idleAndPendingLease();
        readinessAndStop();
        failures();
        leasesAndGenerations();
        timestamps();
        senderQueues();
        System.out.println("PASS: opt-in/off lifecycle, release-before-terminal, silencing/errors/overflow, generation isolation, privacy leases, capture timestamps and bounded priority transport");
    }
}
"""

STUBS = {
    "control/ControlChannel.java": """package com.genymobile.scrcpy.control;
import java.io.IOException;
import java.util.concurrent.LinkedBlockingQueue;
public final class ControlChannel {
 public final LinkedBlockingQueue<DeviceMessage> messages=new LinkedBlockingQueue<>();
 public void send(DeviceMessage message) throws IOException { messages.add(message); }
}""",
    "util/Ln.java": """package com.genymobile.scrcpy.util;
public final class Ln { public static void w(String message) {} public static void d(String message) {} }""",
}

PRODUCTION = [
    "audio/MicrophoneSession.java",
    "audio/AudioTimestampTracker.java",
    "control/DeviceMessage.java",
    "control/DeviceMessageSender.java",
]


def main():
    project = Path(__file__).resolve().parent.parent
    source = project / "server/src/main/java/com/genymobile/scrcpy"
    java_home = os.environ.get("JAVA_HOME")
    javac = str(Path(java_home) / "bin/javac") if java_home else shutil.which("javac")
    java = str(Path(java_home) / "bin/java") if java_home else shutil.which("java")
    if not javac or not java:
        raise SystemExit("A JDK is required: install one or set JAVA_HOME.")
    with tempfile.TemporaryDirectory(prefix="isc-android-mic.") as directory:
        root = Path(directory)
        for name in PRODUCTION:
            destination = root / "com/genymobile/scrcpy" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text((source / name).read_text())
        for name, content in STUBS.items():
            destination = root / "com/genymobile/scrcpy" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
        (root / "AndroidMicTest.java").write_text(JAVA)
        classes = root / "classes"
        subprocess.run([javac, "-d", str(classes),
                        *[str(path) for path in root.rglob("*.java")]], check=True, timeout=30)
        subprocess.run([java, "-ea", "-cp", str(classes), "AndroidMicTest"],
                       check=True, timeout=25)


if __name__ == "__main__":
    main()
