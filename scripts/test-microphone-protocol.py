#!/usr/bin/env python3
"""Test the actual Java microphone/control wire protocol without Android.

Only unrelated pointer-position geometry is stubbed. No audio API, phone,
permission prompt, GUI, network connection or microphone is exercised.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


JAVA = r"""
import com.genymobile.scrcpy.control.ControlMessage;
import com.genymobile.scrcpy.control.ControlMessageReader;
import com.genymobile.scrcpy.control.DeviceMessage;
import com.genymobile.scrcpy.control.DeviceMessageWriter;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public class MicrophoneProtocolTest {
    interface Action { void run() throws Exception; }
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    static byte[] hex(String value) {
        value = value.replace(" ", "");
        byte[] data = new byte[value.length() / 2];
        for (int i = 0; i < data.length; ++i)
            data[i] = (byte) Integer.parseInt(value.substring(i * 2, i * 2 + 2), 16);
        return data;
    }
    static ControlMessage read(byte[] data) throws IOException {
        return new ControlMessageReader(new ByteArrayInputStream(data)).read();
    }
    static byte[] write(DeviceMessage message) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        new DeviceMessageWriter(out).write(message);
        return out.toByteArray();
    }
    static void expectIo(Action action, String message) throws Exception {
        try { action.run(); }
        catch (IOException expected) { return; }
        throw new AssertionError(message);
    }
    static void expectArgument(Action action, String message) throws Exception {
        try { action.run(); }
        catch (IllegalArgumentException expected) { return; }
        throw new AssertionError(message);
    }
    static void checkTruncated(byte[] packet) throws Exception {
        for (int length = 0; length < packet.length; ++length) {
            final byte[] part = Arrays.copyOf(packet, length);
            expectIo(() -> read(part), "Accepted truncated command at " + length);
        }
    }
    public static void main(String[] args) throws Exception {
        int session = 0x89abcdef;
        int sequence = 0xfedcba98;
        byte[] enable = hex("18 01 89abcdef");
        ControlMessage mic = read(enable);
        check(mic.getType() == ControlMessage.TYPE_SET_ANDROID_MIC && mic.getOn()
                && mic.getSession() == session, "Enable command changed session/flag");
        ControlMessage off = read(hex("18 00 89abcdef"));
        check(!off.getOn() && off.getSession() == session, "Disable command changed session");
        check(!read(hex("18 00 00000000")).getOn(), "Stop-all command rejected");
        expectIo(() -> read(hex("18 02 89abcdef")), "Accepted nonboolean enabled flag");
        expectIo(() -> read(hex("18 ff 89abcdef")), "Accepted negative enabled flag");
        expectIo(() -> read(hex("18 01 00000000")), "Accepted zero capture session");
        checkTruncated(enable);

        byte[] query = hex("19 89abcdef fedcba98");
        ControlMessage clock = read(query);
        check(clock.getType() == ControlMessage.TYPE_AUDIO_CLOCK_SYNC
                && clock.getSession() == session
                && clock.getSequence() == Integer.toUnsignedLong(sequence),
                "Clock command lost unsigned sequence/session bits");
        expectIo(() -> read(hex("19 00000000 fedcba98")), "Accepted zero clock session");
        checkTruncated(query);
        ByteArrayOutputStream commands = new ByteArrayOutputStream();
        commands.write(enable);
        commands.write(query);
        commands.write(hex("18 00 89abcdef"));
        ControlMessageReader reader = new ControlMessageReader(new ByteArrayInputStream(commands.toByteArray()));
        check(reader.read().getType() == ControlMessage.TYPE_SET_ANDROID_MIC, "Enable framing failed");
        check(reader.read().getType() == ControlMessage.TYPE_AUDIO_CLOCK_SYNC, "Clock framing failed");
        check(!reader.read().getOn(), "Disable framing failed");
        expectIo(() -> reader.read(), "Reader accepted data beyond EOF");

        check(Arrays.equals(write(DeviceMessage.createMicStatus(session, 2, "é")),
                hex("03 89abcdef 02 0002 c3a9")), "Status wire format changed");
        check(Arrays.equals(write(DeviceMessage.createMicStatus(session, 0, "")),
                hex("03 89abcdef 00 0000")), "Stopped status wire format changed");
        check(Arrays.equals(write(DeviceMessage.createMicStatus(session, 1, "Ready")),
                hex("03 89abcdef 01 0005 5265616479")), "Ready status wire format changed");
        StringBuilder oversized = new StringBuilder();
        for (int i = 0; i < 257; ++i) oversized.append('é');
        oversized.append('x');
        byte[] limited = write(DeviceMessage.createMicStatus(session, 2, oversized.toString()));
        check(limited.length == 8 + 512, "Status exceeds its bounded protocol size");
        check(ByteBuffer.wrap(limited, 6, 2).getShort() == 512, "Status length prefix is incorrect");
        String decoded = new String(limited, 8, 512, StandardCharsets.UTF_8);
        check(decoded.length() == 256 && decoded.indexOf('\ufffd') == -1,
                "Truncated a UTF-8 status codepoint");

        long pts = 0x0102030405060708L;
        byte[] samples = hex("3412 ccff");
        check(Arrays.equals(write(DeviceMessage.createMicPcm(session, pts, samples)),
                hex("04 89abcdef 0102030405060708 0004 3412ccff")),
                "PCM header or little-endian sample bytes changed");
        byte[] maximum = new byte[1920];
        for (int i = 0; i < maximum.length; ++i) maximum[i] = (byte) i;
        byte[] pcm = write(DeviceMessage.createMicPcm(session, 0, maximum));
        check(pcm.length == 15 + 1920 && ByteBuffer.wrap(pcm, 13, 2).getShort() == 1920,
                "Maximum PCM frame was not bounded correctly");
        check(Arrays.equals(Arrays.copyOfRange(pcm, 15, pcm.length), maximum), "PCM payload changed");
        expectArgument(() -> DeviceMessage.createMicPcm(0, 1, samples), "Accepted zero PCM session");
        expectArgument(() -> DeviceMessage.createMicPcm(session, -1, samples), "Accepted negative PCM PTS");
        expectArgument(() -> DeviceMessage.createMicPcm(session, 1, new byte[0]), "Accepted empty PCM");
        expectArgument(() -> DeviceMessage.createMicPcm(session, 1, new byte[3]), "Accepted partial PCM sample");
        expectArgument(() -> DeviceMessage.createMicPcm(session, 1, new byte[1922]), "Accepted oversized PCM");

        check(Arrays.equals(write(DeviceMessage.createAudioClock(session, sequence, pts)),
                hex("05 89abcdef fedcba98 0102030405060708")), "Clock reply wire format changed");
        check(Arrays.equals(write(DeviceMessage.createAudioClock(session, 0, Long.MAX_VALUE)),
                hex("05 89abcdef 00000000 7fffffffffffffff")), "Clock reply truncated 64-bit PTS");
        System.out.println("PASS: microphone/clock command framing, unsigned IDs, malformed flags/sessions, truncated input, status UTF-8 bounds, PCM size/PTS validation and exact wire bytes");
    }
}
"""

PRODUCTION = [
    "control/ControlMessage.java",
    "control/ControlMessageReader.java",
    "control/ControlProtocolException.java",
    "control/DeviceMessage.java",
    "control/DeviceMessageWriter.java",
    "util/Binary.java",
    "util/StringUtils.java",
]


def main():
    project = Path(__file__).resolve().parent.parent
    source = project / "server/src/main/java/com/genymobile/scrcpy"
    java_home = os.environ.get("JAVA_HOME")
    javac = str(Path(java_home) / "bin/javac") if java_home else shutil.which("javac")
    java = str(Path(java_home) / "bin/java") if java_home else shutil.which("java")
    if not javac or not java:
        raise SystemExit("A JDK is required: install one or set JAVA_HOME.")
    with tempfile.TemporaryDirectory(prefix="isc-mic-protocol.") as directory:
        root = Path(directory)
        for name in PRODUCTION:
            destination = root / "com/genymobile/scrcpy" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text((source / name).read_text())
        position = root / "com/genymobile/scrcpy/model/Position.java"
        position.parent.mkdir(parents=True, exist_ok=True)
        position.write_text(
            "package com.genymobile.scrcpy.model; public final class Position {"
            " public Position(int x,int y,int width,int height) {} }"
        )
        (root / "MicrophoneProtocolTest.java").write_text(JAVA)
        classes = root / "classes"
        subprocess.run([javac, "-d", str(classes),
                        *[str(path) for path in root.rglob("*.java")]], check=True, timeout=30)
        subprocess.run([java, "-ea", "-cp", str(classes), "MicrophoneProtocolTest"],
                       check=True, timeout=15)


if __name__ == "__main__":
    main()
