#!/bin/sh
# Build both components locally; the server input to Meson is generated above.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
export PATH="$project_dir/../.build-tools/bin:$PATH"
if test "$(uname -s)" = Darwin; then
  : "${MACOSX_DEPLOYMENT_TARGET:=$(sw_vers -productVersion | cut -d. -f1-2)}"
  export MACOSX_DEPLOYMENT_TARGET
  scripts/build-macos-deps.sh
  export PKG_CONFIG_PATH="$project_dir/build-deps/prefix/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
fi
: "${ANDROID_HOME:=$HOME/Library/Android/sdk}"
export ANDROID_HOME
mkdir -p build-server
BUILD_DIR="$project_dir/build-server" bash server/build_without_gradle.sh
meson setup build --buildtype=debug \
  -Dprebuilt_server="$project_dir/build-server/scrcpy-server" \
  -Dvideotoolbox=false \
  --prefix="$project_dir/local" --reconfigure --clearcache
meson compile -C build -j 2
if test "$(uname -s)" = Darwin; then
  python3 scripts/macos-dependencies.py --target "$MACOSX_DEPLOYMENT_TARGET" \
    --require-prefix "$project_dir/build-deps/prefix" \
    build/app/InvalidScreenCopy build/app/InvalidScreenCopyLauncher
fi
meson test -C build --print-errorlogs --num-processes 1
