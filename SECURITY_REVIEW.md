# InvalidScreenCopy source review

Reviewed 2026-11-08. Upstream baseline: Genymobile/scrcpy v5.0.1,
commit `a0c27ff382c411af1301dcc1cd8205c9aeb81996`.

## Finding

The targeted manual source review found no advertising/analytics SDK, remote
upload endpoint, automatic contacts/SMS/photo harvesting, boot persistence, or
downloaded executable payload in the reviewed Android server paths. This is not
a security certification or a guarantee that the code, device, compiler, SDK,
ADB, or native dependencies are free of vulnerabilities or compromise.

## What was traced

- `server/src/main/AndroidManifest.xml` is an empty manifest. The desktop pushes
  a temporary DEX/JAR and runs `com.genymobile.scrcpy.Server` with `app_process`
  through the already-authorized ADB shell. It does not install a normal APK.
- `device/DesktopConnection.java` uses Android local abstract sockets. Video,
  audio, control replies, and device metadata pass through the ADB tunnel to
  the connected host. No Java IP socket, HTTP client, or external destination
  was found by source-wide searches and inspection of these transport paths.
- `Server.java`, `video/SurfaceEncoder.java`, and the screen/camera capture
  implementations route encoded frames to that transport. Screen is the
  default; camera capture is an explicit option.
- `audio/AudioSource.java` and the direct/playback capture implementations
  support device output and explicit microphone/call-source modes. The default
  audio source is device output, not the microphone. Audio capture is sensitive
  even when its only destination is the user's computer.
- `control/Controller.java`, `DeviceMessageSender`, and device wrappers handle
  input, clipboard, display power/rotation, app launching/listing, UHID, and
  media scanning. Clipboard autosync sends changed text to the host when
  enabled. Disable it with `--no-clipboard-autosync`, or disable all control
  with `--no-control`. App listing can disclose installed application names.
- `util/Command.java` calls were traced: the active command call site obtains
  display information using `dumpsys display`. `CleanUp` spawns its own
  `app_process` helper, removes its own JAR, and restores requested settings.
  Reflection and system library loading expose Android private APIs required
  for screen/control behavior; they do not load a downloaded plugin.
- Content-provider wrappers use Android settings. File I/O found in the Java
  paths covers logging to standard output/error, self-removal, media scan
  requests, and the UHID device. The desktop file-pusher installs/pushes files
  explicitly dropped by the user; it is not an automatic phone-file uploader.
- Desktop `server.c`, `adb/adb_tunnel.c`, and `file_pusher.c` were reviewed for
  launch and transport behavior. Network ADB is supported when explicitly
  configured. The test used USB. ADB authorization itself grants broad shell
  access and remains a separate trust boundary.

## Build and dependencies

The server was compiled from the checked-out Java and AIDL source using the
existing Android API 36/build-tools 36.0.0, javac 22.0.2, and D8. No upstream
precompiled scrcpy server was downloaded or reused. `server/build.gradle` has
only JUnit as a test dependency; the manual build has no third-party runtime
Java dependency. Gradle's wrapper checksum is pinned, but the manual build
avoids the Gradle/plugin dependency graph entirely.

The desktop was compiled with Apple clang 21, Meson 1.12.1, and Ninja 1.13.2.
It dynamically uses the existing Homebrew FFmpeg 9.0.2, SDL 3.4.20, and libusb
1.0.30, plus system frameworks and the existing Android SDK ADB. Their full
source and transitive dependencies were **not audited or rebuilt**. Building
scrcpy from source does not eliminate those supply-chain trust assumptions.

## Device evidence and limits

Before replacement, `pm list packages com.genymobile.scrcpy` returned no package
and `/data/local/tmp/scrcpy-server.jar` was already absent. The source client
uses its own temporary path, `/data/local/tmp/InvalidScreenCopy-server.jar`,
so it does not overwrite a running Homebrew server's path.

The baseline source-built client/server successfully mirrored the attached
Pixel 8 Pro (Android 17), rendered through Metal/VideoToolbox, and finalized a
local MP4. ffprobe reported H.264, 1008x2244, 1,213 frames, 11.652078 seconds of
video during the 20-second run (the screen subsequently became inactive).
All 12 upstream desktop tests passed. Test footage remains local and is not
committed or uploaded.

This was a targeted manual data-flow and build review, not an exhaustive
line-by-line audit, fuzzing campaign, exploit assessment, independent binary
reproducibility proof, CVE inventory, or packet-capture analysis. It cannot
establish that this phone was never infected by other software. No evidence
of a persistent scrcpy infection was found in the checks performed.

## Fork changes reviewed

The fork adds one no-payload control message (23) that refreshes the existing
video capture/encoder to start a new clip on a keyframe. It adds no socket,
permission, data source, service, or persistence to Android. Desktop Record
also sends the existing wake-up key action when control is enabled. Client and
server identify themselves as `5.0.1-invalid.1`, preventing accidental pairing
with the unmodified upstream 5.0.1 server.

The macOS UI stores only the selected recording-directory path in local
NSUserDefaults. MP4 creation uses an exclusive local file descriptor and 0600
permissions. Neither new desktop module uploads clips or performs networking.
Failures are reported locally; partial files are retained if any video was
captured. Recorder changes were checked for stream lifetime, mutex ordering,
exclusive output ownership, finalization errors, and keyframe/audio boundaries.
See [verification](VERIFICATION.md) for automated and device test evidence.

The native macOS device picker runs the existing ADB executable with
`devices -l`, and starts the bundled display with the selected serial as a
separate argument. It never builds a shell command from device names or
identifiers, pairs a wireless device, or accepts debugging authorization for
the user. An explicit `ADB` executable override is honored. Discovery and
display logs stay on this Mac; temporary display logs have mode 0600 and are
removed when the display exits. The last part of a failed display's log is
shown locally for diagnosis. Closing the display requests SDL's regular quit
path over a private pipe. The display and picker share the existing local
recording-folder preference; Android code and capture permissions are unchanged.
