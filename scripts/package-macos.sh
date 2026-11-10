#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
test -f build-deps/completed || { echo 'Build private dependencies first: scripts/build-invalidscreencopy.sh' >&2; exit 1; }
target=$(sw_vers -productVersion)
# Validate before copying anything over a working installation. In particular,
# a library for a newer OS must not be "fixed" by editing its Mach-O version.
python3 scripts/macos-dependencies.py --target "$target" \
    --require-prefix "$project_dir/build-deps/prefix" \
    build/app/InvalidScreenCopy build/app/InvalidScreenCopyLauncher
mkdir -p dist
stage=$(mktemp -d "$project_dir/dist/.package.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM
bundle="$stage/InvalidScreenCopy.app"
display="$bundle/Contents/Helpers/InvalidScreenCopyDisplay.app"
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources" "$display/Contents/MacOS"
cp build/app/InvalidScreenCopy "$display/Contents/MacOS/InvalidScreenCopy"
cp build/app/InvalidScreenCopyLauncher "$bundle/Contents/MacOS/InvalidScreenCopyLauncher"
cp build-server/scrcpy-server "$bundle/Contents/Resources/scrcpy-server"
cp app/data/scrcpy.png app/data/disconnected.png "$bundle/Contents/Resources/"
# Remove the previous generated shell entry point after installing the native
# launcher. The standalone ./InvalidScreenCopy command remains unchanged.
rm -f "$bundle/Contents/MacOS/launch" "$bundle/Contents/MacOS/InvalidScreenCopy"
cat > "$display/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.technicallyinvalid.InvalidScreenCopy.display</string>
<key>CFBundleName</key><string>InvalidScreenCopy Display</string>
<key>CFBundleDisplayName</key><string>InvalidScreenCopy Display</string>
<key>CFBundleExecutable</key><string>InvalidScreenCopy</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>5.0.1</string>
<key>CFBundleVersion</key><string>3</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
cat > "$bundle/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.technicallyinvalid.InvalidScreenCopy</string>
<key>CFBundleName</key><string>InvalidScreenCopy</string>
<key>CFBundleDisplayName</key><string>InvalidScreenCopy</string>
<key>CFBundleExecutable</key><string>InvalidScreenCopyLauncher</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>5.0.1</string>
<key>CFBundleVersion</key><string>3</string>
<key>NSPrincipalClass</key><string>NSApplication</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
python3 scripts/macos-dependencies.py --target "$target" \
    --require-prefix "$project_dir/build-deps/prefix" \
    --bundle "$bundle/Contents/Frameworks" \
    "$display/Contents/MacOS/InvalidScreenCopy" \
    "$bundle/Contents/MacOS/InvalidScreenCopyLauncher"
# Include exact unmodified source archives and licenses for the libraries we
# distribute, along with the recipe needed to rebuild or replace them.
mkdir -p "$bundle/Contents/Resources/Sources" "$bundle/Contents/Resources/Licenses"
for archive in SDL3-3.4.20.tar.gz ffmpeg-9.0.2.tar.xz dav1d-1.5.4.tar.bz2 libusb-1.0.30.tar.bz2; do
    cp "build-deps/downloads/$archive" "$bundle/Contents/Resources/Sources/"
done
cp scripts/build-macos-deps.sh "$bundle/Contents/Resources/Sources/"
cp build-deps/SDL3-3.4.20/LICENSE.txt "$bundle/Contents/Resources/Licenses/SDL.txt"
cp build-deps/dav1d-1.5.4/COPYING "$bundle/Contents/Resources/Licenses/dav1d.txt"
cp build-deps/libusb-1.0.30/COPYING "$bundle/Contents/Resources/Licenses/libusb.txt"
cp build-deps/ffmpeg-9.0.2/COPYING.LGPLv2.1 "$bundle/Contents/Resources/Licenses/FFmpeg-LGPLv2.1.txt"
cp build-deps/ffmpeg-9.0.2/LICENSE.md "$bundle/Contents/Resources/Licenses/FFmpeg.txt"
codesign --force --sign - "$display/Contents/MacOS/InvalidScreenCopy"
codesign --force --sign - "$display"
codesign --force --sign - "$bundle/Contents/MacOS/InvalidScreenCopyLauncher"
python3 scripts/macos-dependencies.py --target "$target" \
    --require-prefix "$bundle/Contents/Frameworks" \
    --manifest "$bundle/Contents/Resources/dependencies.json" \
    "$display/Contents/MacOS/InvalidScreenCopy" \
    "$bundle/Contents/MacOS/InvalidScreenCopyLauncher"
codesign --force --sign - "$bundle"
codesign --verify --deep --strict "$bundle"
if test -e dist/InvalidScreenCopy.app; then
    backup=$(mktemp -d "$project_dir/dist/previous.XXXXXX")
    mv dist/InvalidScreenCopy.app "$backup/"
    printf 'Previous bundle preserved in %s\n' "$backup"
fi
mv "$bundle" dist/InvalidScreenCopy.app
printf 'Built %s/dist/InvalidScreenCopy.app\n' "$project_dir"
