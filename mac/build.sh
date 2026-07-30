#!/usr/bin/env bash
# Build claudemon.app — a native SwiftUI menu bar companion. No Xcode project;
# compiles main.swift with swiftc and assembles a self-contained .app bundle
# (with the Python tailer inside). Ad-hoc signed so it runs locally.
set -euo pipefail
cd "$(dirname "$0")"

APP="claudemon.app"
BIN="claudemon"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

echo "compiling..."
swiftc -O -parse-as-library main.swift \
  -target arm64-apple-macos14.0 \
  -o "$APP/Contents/MacOS/$BIN"

# Bundle the tailer so the app is self-contained; it's run via /usr/bin/python3.
cp ../host/claudemon-tailer.py "$APP/Contents/Resources/claudemon-tailer.py"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>claudemon</string>
  <key>CFBundleIdentifier</key><string>com.claudemon.companion</string>
  <key>CFBundleName</key><string>claudemon</string>
  <key>CFBundleDisplayName</key><string>claudemon</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>LSUIElement</key><true/>
  <key>NSLocalNetworkUsageDescription</key><string>claudemon reaches your device on your local network to show its status.</string>
</dict></plist>
PLIST

# Ad-hoc code sign (lets Gatekeeper run it locally).
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true

echo "built ./$APP"
echo "run:   open ./$APP        (icon appears in the menu bar)"
