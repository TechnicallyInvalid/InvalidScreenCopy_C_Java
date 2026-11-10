#!/bin/sh
# Synthetic-only microphone recordings: no capture devices, phones, or prompts.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
: "${ISC_TEST_FIXTURE:=$project_dir/app/tests/data/recording-fixture.mp4}"
test -f "$ISC_TEST_FIXTURE" || { echo 'Synthetic H.264/AAC fixture is missing; set ISC_TEST_FIXTURE.' >&2; exit 1; }
: "${FFMPEG:=$project_dir/build-deps/prefix/bin/ffmpeg}"
: "${FFPROBE:=$project_dir/build-deps/prefix/bin/ffprobe}"
export FFMPEG FFPROBE
if [ "$(uname -s)" = Darwin ]; then
  python3 scripts/macos-dependencies.py "$(command -v "$FFMPEG")" "$(command -v "$FFPROBE")"
fi
test_dir=$(mktemp -d "$project_dir/build/microphone-recording-test.XXXXXX")
build/app/test_recording_microphones "$ISC_TEST_FIXTURE" "$test_dir"
python3 - "$test_dir" <<'PY'
import json, math, os, pathlib, subprocess, sys, wave
from array import array

ffmpeg, ffprobe = os.environ['FFMPEG'], os.environ['FFPROBE']
files = sorted(pathlib.Path(sys.argv[1]).glob('*.mp4'))
assert len(files) == 3, files
for mask, path in zip((2, 4, 6), files):
    metadata = json.loads(subprocess.check_output([ffprobe, '-v', 'error',
        '-show_streams', '-show_format', '-of', 'json', str(path)]))
    streams = {s['codec_type']: s for s in metadata['streams']}
    video, audio = streams['video'], streams['audio']
    assert video['codec_name'] == 'h264' and int(video['nb_frames']) == 1
    assert audio['codec_name'] == 'aac' and audio['sample_rate'] == '48000'
    assert audio['channels'] == 2
    video_end = float(video.get('start_time', 0)) + float(video['duration'])
    audio_end = float(audio.get('start_time', 0)) + float(audio['duration'])
    assert .45 < float(video['duration']) < .65, metadata
    # One 20 ms producer block plus AAC frame quantization must remain bounded;
    # the simulated 400 ms capture cleanup must not extend the recorded clip.
    assert abs(audio_end - video_end) < .05, (audio_end, video_end)
    packets = json.loads(subprocess.check_output([ffprobe, '-v', 'error',
        '-show_packets', '-show_entries', 'packet=stream_index,pts,dts',
        '-of', 'json', str(path)]))['packets']
    previous = {}
    for packet in packets:
        index, pts, dts = int(packet['stream_index']), int(packet['pts']), int(packet['dts'])
        if index in previous:
            assert pts > previous[index][0] and dts > previous[index][1], packet
        previous[index] = pts, dts
    subprocess.run([ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-threads', '1',
        '-i', str(path), '-fps_mode', 'passthrough', '-f', 'null', '-'], check=True)
    decoded = path.with_suffix('.wav')
    subprocess.run([ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-threads', '1',
        '-i', str(path), '-vn', '-c:a', 'pcm_s16le', '-f', 'wav', str(decoded)], check=True)
    with wave.open(str(decoded), 'rb') as source:
        assert source.getframerate() == 48000 and source.getnchannels() == 2
        assert source.getsampwidth() == 2
        pcm = array('h', source.readframes(source.getnframes()))
    if sys.byteorder != 'little':
        pcm.byteswap()
    # Avoid codec priming and short stop padding when checking the steady tones.
    mono = [(pcm[i] + pcm[i + 1]) / 65536 for i in range(0, len(pcm), 2)][2400:-2400]
    assert len(mono) > 12000
    def amplitude(frequency):
        step = 2 * math.pi * frequency / 48000
        real = sum(value * math.cos(step * i) for i, value in enumerate(mono))
        imag = sum(value * math.sin(step * i) for i, value in enumerate(mono))
        return 2 * math.hypot(real, imag) / len(mono)
    levels = {frequency: amplitude(frequency) for frequency in (440, 800, 1200)}
    assert levels[440] > .025, ('device output missing', levels)
    for frequency, selected in ((800, mask & 2), (1200, mask & 4)):
        assert (levels[frequency] > .02 if selected else levels[frequency] < .008), (mask, levels)
    print(f'PASS: mask={mask}: decoded H.264 + stereo AAC, selected tones present, off sources absent; '
          f'video end={video_end:.6f}s audio end={audio_end:.6f}s; amplitudes={levels}')
print('PASS: synthetic Android-only, local-only, both; monotonic timestamps; delayed cleanup excluded from video duration')
PY
printf 'Synthetic test evidence: %s\n' "$test_dir"
