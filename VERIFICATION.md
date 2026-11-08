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
- After feedback about unreliable header clicks, Record and Stop were moved
  onto the lower status row with the status to their right. That row disables
  window dragging. Actual mouse clicks at the centers and near the edges of
  the buttons created two consecutive clips; both H.264/AAC files fully decoded
  with `ffmpeg -xerror`. The rebuilt app passed signature verification and was
  left running after Stop finalized the second clip.
- The subsequent folder-controls update places Choose Folder and Open Folder
  beside Record/Stop, with status centered in the header. Mouse clicks opened
  the chooser and the current destination in Finder. Chooser cancellation
  preserved the destination; selecting another folder updated the destination
  and tooltip, and Recording → Open Current Folder opened that selected folder.
  The original launch-folder destination was restored after verification.
  The updated client compiled and its packaged signature passed verification.

Phone footage and generated fixtures remain local and are ignored by Git.
The device's audio track decoded successfully; the audible test signal was
synthetic, not a separately played sound on the shared phone. No Thrive app
was opened, installed, removed or navigated during these checks.

## Audio investigation — 2026-11-08

The reported muffled/slow audio has **not been reproduced or fixed**. The
reviewed user clip contains 48 kHz stereo AAC-LC at approximately 128 kb/s.
Its 12,840 readable audio packets have uniformly spaced 1,024-sample timestamps;
there is no container-level slowdown visible in that measurement.

A temporary Java AudioTrack probe played quiet, generated 440 Hz, 1 kHz and
8 kHz tones on the attached Pixel. Each tone lasted two seconds. The unchanged
source-built client recorded device output as AAC at 128 kb/s, without live
audio playback. No microphone input or phone-app navigation was used.

| Source PCM rate | Measured tone frequencies (Hz) | Measured tone lengths | 8 kHz gain relative to 1 kHz |
| --- | --- | --- | --- |
| 48 kHz | 439.9984 / 1000.0001 / 8000.0004 | 2.010 s each | -0.064 dB |
| 24 kHz | 439.9969 / 1000.0073 / 7999.9884 | 2.000 s each | -0.008 dB |

Apple's command-line audio decoder also decoded the 48 kHz test with the same
pitch, duration and relative level. These small tests do not exercise sustained
high-frame-rate video load or prove that live playback and every source app are
unaffected. The first probe completed its tones but its process timed out during
shutdown; the corrected second probe exited successfully. Both temporary probe
uploads were removed. The first capture initialized a renderer despite having
video disabled; the second explicitly used `--no-window`.

The recording integration test now models the device's sparse audio metadata
and absent packet durations. It verifies exact SHA-256 equality of all accepted
AAC payloads and exact presentation timestamps across consecutive clips, plus
the sample rate, channels and duration. This also exposed a separate limitation:
without an explicit final AAC packet duration, the MP4 edit list can omit the
last approximately 21 ms from normal playback even though the packet is stored.
The packet-integrity check ignores edit lists to inspect all stored packets.
This short tail trim does not explain sustained muffling or slower pitch.

QuickTime's already-open movie and View/Playback Speed menu were inspected;
the selected speed could not be established from the returned UI state. No
playback was started and no speed setting or file was changed. The earlier
display-pipeline panic remains unattributed. Further diagnosis needs a specific
audible example/time and whether the problem occurs in saved files, live
mirroring, or both. Application behavior and audio defaults remain unchanged.

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
