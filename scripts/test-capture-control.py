#!/usr/bin/env python3
"""Exercise CaptureControl request/lifecycle bookkeeping without Android or a GUI.

Minimal Android stubs verify our synchronization, parameter key and reset logic;
they do not verify a real encoder. Also compile the complete Android server.
Requires a JDK on PATH, or JAVA_HOME pointing to one.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

SOURCES = {
    'android/os/Bundle.java': r"""package android.os; public class Bundle { public String key; public int value; public void putInt(String k, int v) { key=k; value=v; } }""",
    'android/media/MediaCodec.java': r"""package android.media;
import android.os.Bundle;
public class MediaCodec {
 public static final String PARAMETER_KEY_REQUEST_SYNC_FRAME="request-sync";
 public int syncRequests, eosRequests; public RuntimeException error; public Runnable duringRequest;
 public void setParameters(Bundle b) {
  if (!PARAMETER_KEY_REQUEST_SYNC_FRAME.equals(b.key) || b.value!=0) throw new AssertionError("Incorrect sync-frame parameters");
  if (duringRequest!=null) duringRequest.run();
  if (error!=null) throw error;
  syncRequests++;
 }
 public void signalEndOfInputStream() { eosRequests++; }
}""",
    'com/genymobile/scrcpy/util/Ln.java': r"""package com.genymobile.scrcpy.util; public class Ln { public static int warnings; public static void w(String m, Throwable t) { warnings++; } }""",
    'CaptureControlTest.java': r"""import android.media.MediaCodec;
import com.genymobile.scrcpy.video.CaptureControl;
import com.genymobile.scrcpy.util.Ln;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
public class CaptureControlTest {
 static void check(boolean b, String m) { if (!b) throw new AssertionError(m); }
 static void await(CountDownLatch l) { try { check(l.await(2, TimeUnit.SECONDS), "Latch timed out"); } catch(InterruptedException e) { throw new AssertionError(e); } }
 public static void main(String[] args) throws Exception {
  CaptureControl running=new CaptureControl(); MediaCodec first=new MediaCodec();
  running.setRunningMediaCodec(first); running.requestKeyFrame();
  check(first.syncRequests==1 && first.eosRequests==0 && !running.isResetRequested(), "Request restarted capture");
  CaptureControl starting=new CaptureControl(); starting.requestKeyFrame(); starting.requestKeyFrame();
  MediaCodec started=new MediaCodec(); starting.setRunningMediaCodec(started);
  check(started.syncRequests==1, "Startup requests not coalesced/delivered");
  running.reset(CaptureControl.RESET_REASON_CLIENT_RESIZED); running.requestKeyFrame();
  check(first.syncRequests==1 && first.eosRequests==1, "Requested a dying codec or changed resize reset");
  running.setRunningMediaCodec(null);
  check(running.consumeReset()==CaptureControl.RESET_REASON_CLIENT_RESIZED, "Lost actual resize reason");
  MediaCodec replacement=new MediaCodec(); running.setRunningMediaCodec(replacement);
  check(replacement.syncRequests==1 && replacement.eosRequests==0, "Lost request during resize");
  running.setRunningMediaCodec(null); running.requestKeyFrame(); MediaCodec later=new MediaCodec(); running.setRunningMediaCodec(later);
  check(later.syncRequests==1 && replacement.syncRequests==1, "Request hit detached codec");
  CaptureControl failing=new CaptureControl(); MediaCodec unsupported=new MediaCodec(); unsupported.error=new IllegalArgumentException("unsupported");
  failing.setRunningMediaCodec(unsupported); failing.requestKeyFrame();
  check(Ln.warnings==1 && !failing.isResetRequested() && unsupported.eosRequests==0, "Unsupported request not safely reported");
  failing.setRunningMediaCodec(null); MediaCodec retry=new MediaCodec(); failing.setRunningMediaCodec(retry);
  check(retry.syncRequests==1, "Failed request not retained for replacement");
  CaptureControl concurrent=new CaptureControl(); MediaCodec held=new MediaCodec();
  CountDownLatch entered=new CountDownLatch(1), finish=new CountDownLatch(1), withdrawing=new CountDownLatch(1);
  held.duringRequest=()->{ entered.countDown(); await(finish); }; concurrent.setRunningMediaCodec(held);
  Thread request=new Thread(concurrent::requestKeyFrame); request.start(); await(entered);
  Thread withdraw=new Thread(()->{ withdrawing.countDown(); concurrent.setRunningMediaCodec(null); }); withdraw.start(); await(withdrawing);
  long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(2);
  while (withdraw.getState()!=Thread.State.BLOCKED && System.nanoTime()<deadline) Thread.yield();
  check(withdraw.getState()==Thread.State.BLOCKED, "Codec withdrawal overlapped parameter request");
  finish.countDown(); request.join(2000); withdraw.join(2000);
  check(!request.isAlive() && !withdraw.isAlive() && held.syncRequests==1, "Concurrent lifecycle did not finish");
  concurrent.requestKeyFrame(); check(held.syncRequests==1, "Used released codec after withdrawal");
  System.out.println("PASS: direct sync request, startup coalescing, real resize preservation, detach/replacement, unsupported parameter logging/retry, lifecycle serialization");
 }
}""",
}


def main():
    project = Path(__file__).resolve().parent.parent
    java_home = os.environ.get("JAVA_HOME")
    javac = str(Path(java_home) / "bin/javac") if java_home else shutil.which("javac")
    java = str(Path(java_home) / "bin/java") if java_home else shutil.which("java")
    if not javac or not java:
        raise SystemExit("A JDK is required: install one or set JAVA_HOME.")
    with tempfile.TemporaryDirectory(prefix="isc-keyframe-tests.") as directory:
        root = Path(directory)
        for name, content in SOURCES.items():
            destination = root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
        relative = "com/genymobile/scrcpy/video/CaptureControl.java"
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text((project / "server/src/main/java" / relative).read_text())
        classes = root / "classes"
        subprocess.run([javac, "-d", str(classes),
                        *[str(path) for path in root.rglob("*.java")]], check=True)
        subprocess.run([java, "-ea", "-cp", str(classes), "CaptureControlTest"], check=True)


if __name__ == "__main__":
    main()
