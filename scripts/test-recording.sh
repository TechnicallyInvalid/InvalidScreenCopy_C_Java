#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
: "${FFMPEG:=$project_dir/build-deps/prefix/bin/ffmpeg}"
: "${FFPROBE:=$project_dir/build-deps/prefix/bin/ffprobe}"
: "${ISC_TEST_FIXTURE:=$project_dir/app/tests/data/recording-fixture.mp4}"
export FFMPEG FFPROBE
if [ "$(uname -s)" = Darwin ]; then
  # Inspect before executing external tools: their transitive dependencies can
  # also require a newer OS, even when the client itself is compatible.
  python3 scripts/macos-dependencies.py "$(command -v "$FFMPEG")" "$(command -v "$FFPROBE")"
fi
test_dir=$(mktemp -d "$project_dir/build/recording-test.XXXXXX")
mkdir "$test_dir/clips"
if [ -n "${ISC_TEST_FIXTURE:-}" ]; then
  # The checked-in synthetic fixture works with the private FFmpeg tools,
  # without loading a system H.264 encoder dependency.
  cp "$ISC_TEST_FIXTURE" "$test_dir/fixture.mp4"
else
  "$FFMPEG" -nostdin -v error -f lavfi -i testsrc2=size=320x240:rate=30 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 \
    -c:v libx264 -threads:v 1 -bf 0 -g 15 -c:a aac -ac 2 "$test_dir/fixture.mp4"
fi
build/app/test_recording "$test_dir/fixture.mp4" "$test_dir/clips"
python3 - "$test_dir/clips" "$test_dir/fixture.mp4" <<'PY'
import json, os, pathlib, subprocess, sys
ffmpeg, ffprobe = os.environ['FFMPEG'], os.environ['FFPROBE']

def audio_packets(path, ignore_edits=False):
    command = [ffprobe, '-v', 'error']
    if ignore_edits:
        command += ['-ignore_editlist', '1']
    return json.loads(subprocess.check_output(command + [
        '-select_streams', 'a:0', '-show_packets', '-show_data_hash', 'sha256',
        '-show_entries', 'packet=pts,data_hash', '-of', 'json', str(path)]))['packets']

# The fixture starts with a video keyframe at PTS 0. The negative AAC priming
# packet precedes that boundary and is deliberately omitted from each clip.
expected_audio = [p for p in audio_packets(sys.argv[2]) if int(p['pts']) >= 0]
files = sorted(pathlib.Path(sys.argv[1]).glob('*.mp4'))
assert len(files) == 5, files
for path in files:
    result = json.loads(subprocess.check_output([ffprobe, '-v', 'error',
        '-show_streams', '-show_format', '-of', 'json', str(path)]))
    streams = {s['codec_type']: s for s in result['streams']}
    assert streams['video']['codec_name'] == 'h264'
    video_only = '-43-999' in path.name
    if video_only:
        assert 'audio' not in streams
    else:
        assert streams['audio']['codec_name'] == 'aac'
        assert streams['audio']['sample_rate'] == '48000'
        assert streams['audio']['channels'] == 2
        assert streams['audio']['time_base'] == '1/48000'
        # Inspect all stored packets independently of MP4 edit-list trimming.
        # Missing final-frame duration is a separate limitation (VERIFICATION.md).
        packets = audio_packets(path, ignore_edits=True)
        # Repeated button recordings must preserve encoded audio and timestamps.
        assert [p['data_hash'] for p in packets] == [p['data_hash'] for p in expected_audio]
        assert [int(p['pts']) for p in packets] == [int(p['pts']) for p in expected_audio]
        assert 1.98 < float(streams['audio']['duration']) < 2.03
    assert int(streams['video']['nb_frames']) == 60
    extended = video_only or '-123-002' in path.name
    duration = float(result['format']['duration'])
    assert (2.9 < duration < 3.3) if extended else (1.8 < duration < 2.3)
    # Decode every frame and audio packet, beyond merely checking MP4 metadata.
    subprocess.run([ffmpeg, '-nostdin', '-v', 'error', '-xerror', '-threads', '1', '-i', str(path),
                    '-fps_mode', 'passthrough', '-f', 'null', '-'], check=True)
    print(f'PASS: {path.name}: 60 video frames, audio={not video_only}, finalized and decoded')
print('PASS: repeated clips, keyframe gate, audio/video-only, still-frame duration, collisions, cancellation, missing folder, EOF, write failure')
print('PASS: exact AAC packet contents, packet timing, sample rate and duration with device-style metadata')
PY
printf 'Test evidence: %s\n' "$test_dir"
