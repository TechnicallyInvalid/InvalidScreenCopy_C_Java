# Synthetic recording fixture

`recording-fixture.mp4` is generated test data: two seconds of a 320x240 test
pattern at 30 fps and a 440 Hz stereo tone at 48 kHz. It contains no phone,
microphone, screen recording or other personal content. It is included under
the repository license so the private decoder/AAC build can run the recording
tests without an external H.264 encoder or unsupported system libraries.

SHA-256: `40da12a21fcccf8584c67776e9af9bda2dfe971f460fb27b731729dbf2bfc603`.

Original generation command (requires a compatible FFmpeg with libx264):

```sh
ffmpeg -nostdin -v error -f lavfi -i testsrc2=size=320x240:rate=30 \
  -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 \
  -c:v libx264 -threads:v 1 -bf 0 -g 15 -c:a aac -ac 2 recording-fixture.mp4
```

Encoder versions can change the resulting bytes. Tests validate content and
timestamps; the hash identifies this exact checked-in fixture.
