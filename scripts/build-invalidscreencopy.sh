#!/bin/sh
# Build both components locally; the server input to Meson is generated above.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
: "${ANDROID_HOME:=$HOME/Library/Android/sdk}"
export ANDROID_HOME
mkdir -p build-server
BUILD_DIR="$project_dir/build-server" bash server/build_without_gradle.sh
meson setup build --buildtype=debug \
  -Dprebuilt_server="$project_dir/build-server/scrcpy-server" \
  --prefix="$project_dir/local" --reconfigure
meson compile -C build
meson test -C build --print-errorlogs
