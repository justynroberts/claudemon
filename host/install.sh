#!/usr/bin/env bash
# claudemon desktop tailer installer (macOS).
# Sets up config, verifies the device is reachable, and installs a launchd
# agent that keeps the tailer running and restarts it on reboot/crash.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TAILER="$REPO_DIR/host/claudemon-tailer.py"
CFG_DIR="$HOME/.config/claudemon"
CFG="$CFG_DIR/tailer.toml"
LABEL="com.claudemon.tailer"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/claudemon-tailer.log"

# Apple's python is the one with macOS 15 Local Network access. A Homebrew
# python gets "No route to host" to the LAN device even though curl works.
PY=/usr/bin/python3

echo "== claudemon tailer installer =="
[ -x "$PY" ] || { echo "error: $PY not found (need Apple's python3)"; exit 1; }
[ -f "$TAILER" ] || { echo "error: tailer not found at $TAILER"; exit 1; }

default_url="http://claudemon.local"
read -r -p "Device URL [$default_url]: " url; url="${url:-$default_url}"
url="${url%/}"
read -r -p "Shared secret (shown on the device setup page): " secret
[ -n "$secret" ] || { echo "error: shared secret is required"; exit 1; }
read -r -p "Push interval seconds [10]: " interval; interval="${interval:-10}"

mkdir -p "$CFG_DIR"
cat > "$CFG" <<EOF
device_url    = "$url"
shared_secret = "$secret"
interval      = $interval
EOF
chmod 600 "$CFG"
echo "wrote $CFG"

echo -n "checking device at $url ... "
"$PY" - "$url" <<'PYEOF' || true
import sys, urllib.request, json
try:
    raw = urllib.request.urlopen(sys.argv[1] + "/status", timeout=6).read()
    print("reachable, state =", json.loads(raw).get("state"))
except Exception as e:
    print("NOT reachable (%s)." % e)
    print("  -> finish device WiFi setup and make sure it's on this network;")
    print("     the agent will keep retrying once it's up.")
PYEOF

mkdir -p "$HOME/Library/LaunchAgents" "$(dirname "$LOG")"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array>
    <string>$PY</string>
    <string>$TAILER</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict></plist>
EOF

launchctl unload "$PLIST" 2>/dev/null || true
launchctl load "$PLIST"

echo
echo "installed and started: $LABEL"
echo "  logs:    $LOG"
echo "  config:  $CFG"
echo "  remove:  host/uninstall.sh"
echo
echo "It will start at login and restart automatically. First run only tracks"
echo "usage from now on; it backfills nothing historical."
