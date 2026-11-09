# Build and use InvalidScreenCopy

This fork starts at upstream scrcpy v5.0.1. The C client and Android Java/AIDL
server are both compiled locally. The macOS bridge is Objective-C because
AppKit provides the native buttons and folder chooser; the recording engine
remains C and the device component remains Java.

## Build on macOS

Prerequisites: Xcode command-line tools, Python 3, Java, Android SDK platform 36
and build-tools 36.0.0, and `adb`. Native dependencies: SDL3, FFmpeg, libusb,
and pkg-config (the installed Homebrew copies can be used).

```sh
python3 -m venv .build-tools
.build-tools/bin/pip install meson==1.12.1 ninja==1.13.2
export PATH="$PWD/.build-tools/bin:$PATH"
export ANDROID_HOME="$HOME/Library/Android/sdk"
scripts/build-invalidscreencopy.sh
scripts/test-recording.sh
scripts/package-macos.sh
```

The Java server is built using the reviewed `server/build_without_gradle.sh`
and the installed AIDL, javac and D8 tools. Meson's `prebuilt_server` argument
points to **that locally generated output**, not a downloaded upstream server.
All original Apache-2.0 notices are retained.

Open `dist/InvalidScreenCopy.app` to bring up **Connected Devices**. Select a
ready Android phone and click **Open Display** (or double-click its row).
The list refreshes automatically and shows USB devices, emulators, and devices
already connected through Android wireless debugging. Unlock an unauthorized
phone and accept its debugging prompt before opening its display.

One display runs at a time. **Show Display** brings it forward; **Close Display**
requests a normal shutdown, including finalization of any active recording.
Closing the phone window returns to the picker. Reopening the main app from
Finder or the Dock brings the picker forward even if its window was closed.
The display's application menu also has **Connected Devices…** (Command-0).
Quitting the main app waits for its display to close. Stay-awake is enabled
while mirroring; the previous Android setting is restored at exit.

The standalone `./InvalidScreenCopy --select-usb` command retains the usual
scrcpy options and the shell's working directory. Select a particular device
with `-s SERIAL`. The native launcher also forwards explicit command-line
arguments to the mirror. Native libraries remain external; the bundle is
locally ad-hoc signed, not a notarized standalone distribution.

## Record clips

- Click **Record** or choose **Recording → Start Recording** (Command-R).
- Click **Stop Recording** or press Command-Shift-R. Wait for **Saved** before
  moving a clip. Closing the app also finalizes an active recording.
- Click **Choose Folder** or choose **Recording → Choose Recording Folder…**
  to set a persistent folder.
  Cancel leaves the previous choice untouched. A folder change affects the
  next clip; an active clip stays in its original file.
- Click **Open Folder** or choose **Recording → Open Current Folder** to open
  the selected recording directory in Finder. All six buttons share the second
  header row, with status centered above them and clear of macOS title-bar indicators.
- **Use Launch Folder** clears that preference. The default is the shell's
  working directory; for Finder launches with cwd `/`, it is the folder
  containing the app bundle. Hover over the status/buttons to see full paths.

Names use local Mac time at the Record button press:
`YYYY-MM-DD-HH-MM-SS-MSS.mp4`, with three millisecond digits. If an identical
timestamp already exists, `-001`, `-002`, etc. are appended. Creation uses
`O_EXCL`, including for symlink collisions, and writes through the reserved file
descriptor. Existing recordings are never truncated. New clips have mode 0600.

The Record button wakes an idle phone if control is enabled and refreshes the
video encoder to produce a keyframe even on a static display. The mirror
window, audio stream, and ADB connection stay open. Each clip starts at the
first decodable keyframe after the request; there may be a brief capture delay.
Pre-boundary audio is discarded and timestamps are rebased per clip. Default
audio is device-output AAC for MP4 compatibility; no microphone is enabled.

An empty clip with no keyframe is removed after cancellation or a ten-second
timeout. A write failure keeps any partial file and reports failure; mirroring
continues. A display size change finalizes the current clip; start a new clip
after rotating/resizing. Static screens use variable-frame-rate video; the
final still frame is held through the stop time rather than cutting the clip
short when Android stops sending unchanged frames.

The native controls appear on macOS when video/window playback is enabled and
`--record` is absent. The original `--record=FILE` workflow remains available.
With `--no-control`, the UI cannot wake the screen or refresh the encoder and
must wait for a natural keyframe. Explicit codec overrides must be MP4-compatible.
Other platforms retain the original CLI workflow; their new builds were not
tested in this macOS task.

## Capture screenshots

- **Copy Screenshot** copies the currently displayed phone image to the Mac
  clipboard, with PNG and TIFF representations for pasting into image-aware apps.
- **Save Screenshot** writes a PNG into the same folder selected for videos.
  Names use `YYYY-MM-DD-HH-MM-SS-MSS.png`; collisions add `-001`, `-002`, etc.
  Existing files are never overwritten, and new images have mode 0600.

These are buttons only: no keyboard shortcuts are added or reassigned.
The screenshot contains the displayed phone pixels at the current rendered
resolution, without the native controls or letterbox borders. It uses the
current display texture and its orientation, including the frozen image when
display playback is paused. No new frame or Android screenshot command is
requested. Buttons are disabled until a phone image is available and while a
folder/error dialog is open. Both actions work during video recording.

Recordings remain local. See [the review](SECURITY_REVIEW.md) for the precise
security scope and the remaining dependency and ADB trust assumptions.
