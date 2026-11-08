#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
bundle=dist/InvalidScreenCopy.app
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
cp build/app/InvalidScreenCopy "$bundle/Contents/MacOS/InvalidScreenCopy"
cp build-server/scrcpy-server "$bundle/Contents/Resources/scrcpy-server"
cp app/data/scrcpy.png app/data/disconnected.png "$bundle/Contents/Resources/"
cat > "$bundle/Contents/MacOS/launch" <<'EOF'
#!/bin/sh
set -eu
binary_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export PATH="$HOME/Library/Android/sdk/platform-tools:/opt/homebrew/bin:/usr/local/bin:$PATH"
export SCRCPY_SERVER_PATH="$binary_dir/../Resources/scrcpy-server"
export SCRCPY_ICON_DIR="$binary_dir/../Resources"
if [ "$#" -eq 0 ]; then set -- --select-usb --stay-awake; fi
exec "$binary_dir/InvalidScreenCopy" "$@"
EOF
chmod +x "$bundle/Contents/MacOS/launch"
cat > "$bundle/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.technicallyinvalid.InvalidScreenCopy</string>
<key>CFBundleName</key><string>InvalidScreenCopy</string>
<key>CFBundleDisplayName</key><string>InvalidScreenCopy</string>
<key>CFBundleExecutable</key><string>launch</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>5.0.1</string>
<key>CFBundleVersion</key><string>1</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
codesign --force --sign - "$bundle/Contents/MacOS/InvalidScreenCopy"
codesign --force --sign - "$bundle"
printf 'Built %s/%s\n' "$project_dir" "$bundle"
