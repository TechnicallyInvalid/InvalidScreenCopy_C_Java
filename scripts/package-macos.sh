#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_dir"
bundle=dist/InvalidScreenCopy.app
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
<key>CFBundleVersion</key><string>2</string>
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
<key>CFBundleVersion</key><string>2</string>
<key>NSPrincipalClass</key><string>NSApplication</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
codesign --force --sign - "$display/Contents/MacOS/InvalidScreenCopy"
codesign --force --sign - "$display"
codesign --force --sign - "$bundle/Contents/MacOS/InvalidScreenCopyLauncher"
codesign --force --sign - "$bundle"
printf 'Built %s/%s\n' "$project_dir" "$bundle"
