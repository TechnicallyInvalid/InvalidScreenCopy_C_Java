#!/bin/sh
# Private, source-built runtime libraries. Never changes a Homebrew installation.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
test "$(uname -s)" = Darwin || { echo 'macOS is required' >&2; exit 1; }
export PATH="$project_dir/../.build-tools/bin:$PATH"
: "${MACOSX_DEPLOYMENT_TARGET:=$(sw_vers -productVersion | cut -d. -f1-2)}"
export MACOSX_DEPLOYMENT_TARGET
deps="$project_dir/build-deps"
prefix="$deps/prefix"
mkdir -p "$deps/downloads" "$prefix"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
export CFLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
export LDFLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -Wl,-headerpad_max_install_names"

fetch() {
    name=$1 hash=$2 url=$3
    archive="$deps/downloads/$name"
    if ! test -f "$archive"; then
        curl --fail --location --retry 2 --connect-timeout 20 --max-time 180 \
            --output "$archive.part" "$url"
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$hash" "$archive" | shasum -a 256 --check
}
fetch SDL3-3.4.20.tar.gz f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f \
    https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
fetch ffmpeg-9.0.2.tar.xz 8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e \
    https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
fetch dav1d-1.5.4.tar.bz2 2abfb0c89212e6e4733a54e0ae509ec00a5b845a6360946f918806e14aedb011 \
    https://code.videolan.org/videolan/dav1d/-/archive/1.5.4/dav1d-1.5.4.tar.bz2
fetch libusb-1.0.30.tar.bz2 fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf \
    https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2

# Include the recipe and target in the cache key; changing either rebuilds all
# private dependencies. Pin downloads above even when the build is cached.
key="$(shasum -a 256 "$0" | cut -d' ' -f1):$MACOSX_DEPLOYMENT_TARGET:$(uname -m)"
if test -f "$deps/completed" && test "$(cat "$deps/completed")" = "$key"; then
    echo "Private dependencies already built for macOS $MACOSX_DEPLOYMENT_TARGET"
    exit 0
fi
for archive in "$deps/downloads/"*.tar.*; do tar -xf "$archive" -C "$deps"; done

cmake -S "$deps/SDL3-3.4.20" -B "$deps/sdl-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DCMAKE_SHARED_LINKER_FLAGS="$LDFLAGS" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF
cmake --build "$deps/sdl-build" --parallel 2
cmake --install "$deps/sdl-build"

meson setup "$deps/dav1d-build" "$deps/dav1d-1.5.4" --reconfigure \
    --prefix="$prefix" --libdir=lib --buildtype=release --default-library=shared \
    -Denable_tools=false -Denable_tests=false
meson compile -C "$deps/dav1d-build" -j 2
meson install -C "$deps/dav1d-build"

mkdir -p "$deps/usb-build"
(cd "$deps/usb-build"
    # Sandboxed sysctl may return an empty ARG_MAX. A conservative explicit
    # bound avoids libtool's broken partial-link fallback in that environment.
    lt_cv_sys_max_cmd_len=32768 ../libusb-1.0.30/configure --prefix="$prefix" --disable-static \
        --disable-dependency-tracking
    make -j 2
    make install)

mkdir -p "$deps/ffmpeg-build"
(cd "$deps/ffmpeg-build"
    ../ffmpeg-9.0.2/configure --prefix="$prefix" --enable-shared --disable-static \
        --disable-autodetect --disable-doc --disable-programs --disable-network \
        --disable-avdevice --disable-everything \
        --enable-avcodec --enable-avformat --enable-avutil --enable-swresample \
        --enable-ffmpeg --enable-ffprobe --enable-avfilter --enable-swscale \
        --enable-filter=aresample,format,aformat,null,anull,scale \
        --enable-encoder=wrapped_avframe,pcm_s16le \
        --enable-libdav1d --enable-decoder=h264,hevc,libdav1d,vp8,vp9,aac,opus,flac,pcm_s16le \
        --enable-parser=h264,hevc,av1,vp8,vp9,aac,opus,flac \
        --enable-muxer=mp4,matroska,webm,opus,adts,flac,wav,null \
        --enable-demuxer=mov,matroska --enable-protocol=file \
        --extra-cflags="$CFLAGS" --extra-ldflags="$LDFLAGS" \
        --install-name-dir="$prefix/lib"
    make -j 2
    make install)
printf '%s\n' "$key" > "$deps/completed"
printf 'Private dependencies: %s (macOS %s)\n' "$prefix" "$MACOSX_DEPLOYMENT_TARGET"
