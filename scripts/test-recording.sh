#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
test_dir=$(mktemp -d "$project_dir/build/recording-test.XXXXXX")
mkdir "$test_dir/clips"
ffmpeg -nostdin -v error -f lavfi -i testsrc2=size=320x240:rate=30 \
  -f lavfi -i sine=frequency=440:sample_rate=48000 -t 2 \
  -c:v libx264 -bf 0 -g 15 -c:a aac -ac 2 "$test_dir/fixture.mp4"
build/app/test_recording "$test_dir/fixture.mp4" "$test_dir/clips"
python3 - "$test_dir/clips" <<'PY'
import json, pathlib, subprocess, sys
files = sorted(pathlib.Path(sys.argv[1]).glob('*.mp4'))
assert len(files) == 5, files
for path in files:
    result = json.loads(subprocess.check_output(['ffprobe', '-v', 'error',
        '-show_streams', '-show_format', '-of', 'json', str(path)]))
    streams = {s['codec_type']: s for s in result['streams']}
    assert streams['video']['codec_name'] == 'h264'
    video_only = '-43-999' in path.name
    if video_only:
        assert 'audio' not in streams
    else:
        assert streams['audio']['codec_name'] == 'aac'
    assert int(streams['video']['nb_frames']) == 60
    extended = video_only or '-123-002' in path.name
    duration = float(result['format']['duration'])
    assert (2.9 < duration < 3.3) if extended else (1.8 < duration < 2.3)
    # Decode every frame and audio packet, beyond merely checking MP4 metadata.
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-xerror', '-i', str(path),
                    '-f', 'null', '-'], check=True)
    print(f'PASS: {path.name}: 60 video frames, audio={not video_only}, finalized and decoded')
print('PASS: repeated clips, keyframe gate, audio/video-only, still-frame duration, collisions, cancellation, missing folder, EOF, write failure')
PY
printf 'Test evidence: %s\n' "$test_dir"
