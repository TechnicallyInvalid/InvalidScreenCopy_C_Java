# Device audio and optional microphones

The macOS recording toolbar has independent **Android Mic** and **Local Mic**
checkboxes. Both start **off** each time the display opens. Android device-output
audio remains included; a microphone never replaces it.

| Android Mic | Local Mic | Recording audio |
| --- | --- | --- |
| Off | Off | Android device output |
| On | Off | Device output + Android microphone |
| Off | On | Device output + this Mac's default microphone |
| On | On | Device output + both microphones |

Select the sources before pressing Record. Selections are fixed while a clip
starts, records and finishes; press Stop Recording before changing them.
Selecting a checkbox does not open a microphone. Selected sources open only
for the requested recording, then close before file finalization can block on
storage. Microphone PCM is routed exclusively to recording, never to the live
speaker player. Normal Android-output monitoring remains unchanged.

## Permission and availability

Only explicitly enabling **Local Mic** requests macOS microphone permission.
Neither app launch, device selection nor Record silently requests permission.
Denial/restriction leaves Local Mic off with a status explanation. Permission is
rechecked when a selected clip starts; denial, revocation, disconnection or input
starvation fails the recording visibly. No permission setting is changed by the
application. The app and helper include a microphone usage description.

**Android Mic** uses the already-authorized Android shell capture path, only
after Record with that source selected. The additive implementation requires
Android 12 or newer and a device that permits simultaneous output and microphone
capture. It checks that both captures are active, unsilenced and have usable
hardware timestamps. Unsupported, denied, silenced or unavailable capture fails
visibly. It does not launch a foreground workaround activity, install an APK,
change permissions or substitute the microphone for device output.

The Android microphone has a five-second session heartbeat lease. Stop and
disconnect release capture; the lease also bounds capture if the client freezes
or a broken transport prevents the stop message arriving. Failure to queue a
stop closes the control connection. Stale session messages cannot start, stop or
renew another clip's microphone. No microphone object is opened while off.

Microphone recording requires Android device-output audio and a control
connection. Explicit CLI modes that disable audio/control or choose a microphone
as the sole Android audio source do not provide this additive workflow. The
foreground device picker explicitly selects device output. Existing CLI recording
and non-macOS behavior remain separate from the native toolbar feature.

## Mixing and synchronization

With both microphones off, recording retains the original encoded device-audio
packets. With either microphone selected, the recording path decodes device
audio, aligns the selected PCM streams, mixes to stereo 48 kHz and encodes AAC
at 256 kb/s. Equal gain of `1 / number of included sources` prevents summed full
scale inputs from clipping. The live player receives only the original device
audio. There is no microphone monitor or echo-cancellation feature.

Android microphone PCM carries first-sample timestamps on the same monotonic
clock as video/device audio. Local capture uses a continuous sample counter,
queue-compensated initial timestamp and gradual clock correction. A session
clock exchange estimates the Mac/Android offset using bounded round trips;
periodic probes and bounded resampling correction track drift. All buffers are
bounded. Missing selected audio, excessive drift/discontinuities, queue overflow
or clock expiry fail the clip explicitly instead of silently omitting a source.

Video starts at a keyframe after capture/mixer readiness. Pre-keyframe audio is
discarded. Stop captures a fixed timeline boundary, closes microphones, drains
the common available audio interval and finalizes MP4. A storage or capture
failure preserves any partial video and reports failure; an empty failed clip is
removed. Existing recordings are never overwritten.

Clock exchange cannot measure physical ADC/driver latency. SDL provides local
sample delivery timing rather than a hardware ADC timestamp. Headless tone and
drift tests validate algorithms, not actual microphone quality or physical A/V
synchronization. OEM capture restrictions, Bluetooth/input latency and real
device behavior remain unverified. The prior Mac display panic is unresolved;
this work does not claim to fix it.

## Reproduction and validation scope

Build with `scripts/build-invalidscreencopy.sh`. It builds native AAC support in
the existing hash-pinned private FFmpeg, retains software video decoding, checks
the compatible dependency graph and runs the C/Objective-C tests. Java checks:

```sh
python3 scripts/test-capture-control.py
python3 scripts/test-microphone-protocol.py
python3 scripts/test-android-mic.py
scripts/test-recording.sh
scripts/test-microphone-recording.sh
```

Additional lifecycle, mixer, clock and integration test evidence is recorded in
[VERIFICATION.md](VERIFICATION.md). Tests use synthetic tones/frames, fake
permissions and fake capture devices. No phone, real microphone, permission
dialog, mirroring, live resize or installed application is exercised in this
development pass. Packaging may replace `dist/InvalidScreenCopy.app` while
preserving its previous bundle; it does not install or launch the application.
