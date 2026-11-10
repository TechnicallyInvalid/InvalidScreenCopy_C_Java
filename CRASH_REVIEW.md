# macOS crash investigation — 2026-11-10

The two recent system failures have the same Apple display-pipeline panic:
`mismatched swapID's` at `UnifiedPipeline.cpp:17839`, in a WindowServer task,
with AppleMobileDispT602X-DCP and IOMobileGraphicsFamily in the backtrace.
One occurred during recording and another during resizing without a recording
thread. Recording is therefore not necessary for this failure. This is a
kernel/display-driver assertion, not an ordinary InvalidScreenCopy exception.
The reports do **not** establish which app action or driver defect caused it.
QuickTime is not established as the cause either.

Raw panic reports, device identifiers, user recordings and screenshots are
kept local. This review summarizes relevant findings without publishing them.

## Confirmed defects and changes

- **Unsupported library deployment targets.** The old executable loaded SDL
  3.4.20 and, through Homebrew FFmpeg, SVT-AV1 with minimum macOS 27.2. The host
  is macOS 27.0. A prior successful launch did not make that supported. Private
  source builds now replace the entire non-system runtime dependency graph:
  SDL, a minimal FFmpeg, dav1d and libusb. Downloads are version/hash pinned.
  The build and packager recursively inspect Mach-O minimum OS values and
  dependency resolution and reject incompatible or non-private libraries.
  Packaging embeds the libraries, licenses and exact source archives; no
  global Homebrew installation is modified.
- **Rendering in native resize callbacks.** The inherited macOS event watcher
  could render/present inside AppKit's live-resize handling. macOS now uses the
  normal queued resize event path. The image may pause while dragging and
  refresh afterward. SDL warns that event-watch callbacks can also execute on
  another thread: [SDL_AddEventWatch](https://wiki.libsdl.org/SDL3/SDL_AddEventWatch).
- **Competing native and SDL window sizing.** The custom accessory forced a
  native 806-point minimum/frame width after SDL initialized its aspect-ratio
  constrained window. That mutation is removed. All six buttons stay on the
  lower header line; a horizontal scrollbar makes them reachable in a narrow
  window, with status centered above.
- **Recording stalled the UI and streams.** Opening a destination and joining
  a writer happened on the main thread and/or under the stream mutex. A
  lifecycle worker now owns all recorder file I/O and cleanup. Start, Stop,
  polling and stream delivery stay nonblocking during stalled destination I/O.
  Start remains disabled until finalization finishes. Shutdown stops/hides the
  display and joins streams before waiting for the recording worker.
- **Unbounded recorder queues.** Slow output could accumulate packets without
  a limit. Combined queued payload/packet overhead is capped at 64 MiB and
  8,192 packets. Overflow marks recording as failed and preserves its partial
  file. Native button recording failures do not terminate mirroring. Legacy
  explicit `--record` retains its upstream fatal-recording behavior.
- **Recorder memory/lifetime defects.** Codec extradata now has FFmpeg's
  required zero padding and replaces prior allocations safely. A retained
  frame is freed on write errors. Stopped state is published under the mutex
  before muxer destruction, so a late audio sink cannot access a freed context.
  Failure to write the last frame is reported as failure, not success.
- **Record unnecessarily reset Android capture.** The custom control message
  now requests `MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME` without rebuilding
  capture or the encoder. Requests are synchronized with encoder replacement.
  An idle encoder may need a new screen frame; the existing ten-second deadline
  reports an actionable error and removes a new empty clip if none arrives.
- **Other UI stalls and diagnostics.** Finder opening and screenshot file
  writing are asynchronous. Quit cancels its deferred termination if a child
  remains busy for fifteen seconds, preserving its recording and a responsive
  picker rather than killing it. Discovery recovers after cancelled Quit.
  Abnormal display exits retain a private diagnostic tail (up to five 64 KiB
  logs in `~/Library/Logs/InvalidScreenCopy`). If the app exits before this
  best-effort copy finishes, the original temporary log is retained.

## Compatibility rendering mode and limits

macOS defaults to **OpenGL rendering and software video decoding**, and the
private build disables VideoToolbox integration. This avoids this app's
Metal/VideoToolbox path while investigating the panic. OpenGL still uses the
GPU/system compositor; this is not a completely software display pipeline or
a guarantee against kernel failures. Merely selecting SDL's software renderer
would not establish that guarantee either: SDL's Cocoa window surface can use
an accelerated texture renderer for presentation.

The renderer can still be changed explicitly by CLI/environment; the selected
renderer is logged. Hardware decode is not available in this compatibility
build. The supported screen/audio codecs and recording containers remain
available. Software decode may consume more CPU.

The audit covered custom C/Objective-C/Java changes, renderer/decoder/texture
lifetimes, resize dispatch, recording queues and shutdown, screenshot capture,
launcher lifecycle, and build/package dependencies. Screenshot readback order
and ownership did not reveal a confirmed lifetime bug. PNG conversion and
clipboard operations still run on the main thread; large captures may briefly
pause rendering. This was not an exhaustive audit of macOS, SDL or FFmpeg.

No deliberate kernel-panic reproduction, GUI mirroring, resize stress test,
phone execution, or QuickTime playback is performed in this repair pass.
Headless checks are recorded in [VERIFICATION.md](VERIFICATION.md). They can
validate our code and compatibility checks, but cannot establish that the
underlying system panic is fixed. Actual display/audio/screenshot behavior of
this build still needs cautious normal-use observation. The earlier report of
muffled/slow audio remains unproven and is not claimed fixed by these changes.
