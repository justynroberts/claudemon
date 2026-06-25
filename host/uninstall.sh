#!/usr/bin/env bash
# Remove the claudemon tailer launchd agent. Leaves config in ~/.config/claudemon.
set -euo pipefail
LABEL="com.claudemon.tailer"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
launchctl unload "$PLIST" 2>/dev/null || true
rm -f "$PLIST"
echo "removed $LABEL. Config kept at ~/.config/claudemon/ (delete it to fully reset)."
