# Verification — 2026-11-08 UTC

Upstream base: `a0c27ff382c411af1301dcc1cd8205c9aeb81996` (v5.0.1).
Fork protocol/build version: `5.0.1-invalid.1`.

## Build

- Android server compiled from Java and AIDL with the installed Android 36
  SDK/build-tools 36.0.0, javac 22.0.2, and D8. No precompiled scrcpy server used.
- Desktop C and macOS Objective-C bridge compiled with Apple clang 21,
  Meson 1.12.1 and Ninja 1.13.2 on the Apple Silicon host Upsilonz.
- All 13 Meson tests passed (12 upstream tests plus exclusive-filename tests).
- `scripts/test-recording.sh` passed using generated H.264/AAC fixtures.
- Local ad-hoc app signature passed `codesign --verify --deep --strict`.
- The linker warns that installed Homebrew SDL 3.4.20 targets macOS 27.2 while
  this host reports 27.0. Launch, rendering, audio and recording succeeded here;
  this is not verification on other macOS versions.

## Recording tests

The integration harness creates and fully decodes five MP4s: three consecutive
clips, an EOF-finalized clip, and a video-only clip. Each has 60 synthetic video
frames. Audio clips contain AAC. Tests cover initial non-keyframe and early
audio rejection, individual timestamp rebasing, three-digit milliseconds,
same-millisecond collisions, still-frame duration, stop before the first frame,
missing output directories, EOF, and an injected ENOSPC/full-disk write failure.
The filename test also verifies symlink collision handling, unchanged existing
file contents, and file permissions of 0600.

On the attached Pixel 8 Pro, Android 17, USB:

- Source-built baseline rendered through Metal/VideoToolbox and saved a
  1008x2244 H.264 clip before the Homebrew version was removed.
- Native Record/Stop buttons and their enabled/disabled states were exercised.
- Folder chooser cancellation, selection, persistence across relaunch, and
  **Use Launch Folder** were verified through the native macOS UI.
- Consecutive clips were saved without reconnecting; static-screen capture
  exposed and drove fixes for keyframe acquisition and final-frame duration.
- Final clip `2026-11-07-19-43-38-423.mp4` contained 12 H.264 frames spanning
  **9.676522 s** and 451 AAC packets spanning **9.600000 s**, with audio beginning
  at **0.076521 s**. Its audio and video therefore end together to microsecond
  rounding. Low frame count is expected for an unchanged screen.
- The final clip was fully decoded with `ffmpeg -xerror -fps_mode passthrough`.
- Screen-off/no-keyframe timeout reported failure and removed the empty clip.
- Final visual check showed the buttons separated from native title-bar
  controls and a concise filename/status display.

Phone footage and generated fixtures remain local and are ignored by Git.
The device's audio track decoded successfully; the audible test signal was
synthetic, not a separately played sound on the shared phone. No Thrive app
was opened, installed, removed or navigated during these checks.

## Replacement and cleanup

`brew uninstall --formula scrcpy` succeeded for 5.0.1, with automatic dependency
removal disabled. FFmpeg, SDL3 and libusb remain installed for the new build.
The old mirror was stopped after baseline verification. Android had no
`com.genymobile.scrcpy` package and no old `/data/local/tmp/scrcpy-server.jar`
file to uninstall. Both the old and the fork's temporary server paths were
confirmed absent after self-cleanup. No broad Android deletion was performed.

The locally built app is at `dist/InvalidScreenCopy.app`. Source changes are
on the `invalidscreencopy` branch. See [build/usage](INVALIDSCREENCOPY.md) and the
[security review](SECURITY_REVIEW.md). Neither successful compilation nor these
tests prove absolute security or rebuild the native dependency chain.
