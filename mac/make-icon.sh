#!/usr/bin/env bash
# Regenerate claudemon.icns from icon.html (edit the HTML to change the icon).
# Needs Google Chrome (headless render) + macOS sips/iconutil.
set -euo pipefail
cd "$(dirname "$0")"

CHROME="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
"$CHROME" --headless=new --disable-gpu --no-sandbox --hide-scrollbars \
  --default-background-color=00000000 --window-size=1024,1024 --virtual-time-budget=2500 \
  --screenshot="$PWD/icon_1024.png" "file://$PWD/icon.html"

rm -rf claudemon.iconset && mkdir claudemon.iconset
gen() { sips -z "$2" "$2" icon_1024.png --out "claudemon.iconset/$1" >/dev/null; }
gen icon_16x16.png 16;    gen icon_16x16@2x.png 32
gen icon_32x32.png 32;    gen icon_32x32@2x.png 64
gen icon_128x128.png 128; gen icon_128x128@2x.png 256
gen icon_256x256.png 256; gen icon_256x256@2x.png 512
gen icon_512x512.png 512; gen icon_512x512@2x.png 1024
iconutil -c icns claudemon.iconset -o claudemon.icns
rm -rf claudemon.iconset
echo "wrote claudemon.icns — rebuild the app with ./build.sh"
