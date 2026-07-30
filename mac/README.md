# claudemon — Mac companion

A native SwiftUI **menu bar app** to configure and control the desktop tailer —
no terminal needed. It edits your keys/budgets, starts/stops the tailer, and
shows live device status. The Python tailer is bundled *inside* the app, so it's
self-contained.

## Build & run

```bash
./build.sh          # compiles main.swift -> claudemon.app (self-contained)
open ./claudemon.app
```

The icon appears in the menu bar (top-right). Click it for the panel:

- **Device URL** + **Shared secret** (with reveal) — the values from the device
  setup page.
- **Budgets** (session 5h / week 7d / month 30d, in tokens) + push **interval**.
  These drive the on-device usage gauges (a local proxy for `/usage`).
- **Save** writes `~/.config/claudemon/tailer.toml`; **Start/Stop** loads/unloads
  the launchd agent (auto-starts at login, restarts on crash).
- Status pill + device IP · env count; **Logs** opens the tailer log.

## Notes

- It runs the tailer via **`/usr/bin/python3`** (Apple's — has macOS 15 Local
  Network access). The **app itself** also talks to the device on the LAN, so
  macOS will prompt for **Local Network** permission the first time — allow it,
  or the status pill stays "unreachable" (System Settings → Privacy & Security →
  Local Network).
- It's ad-hoc signed (local use). To move it, drag `claudemon.app` to
  `/Applications`; keep it there so the bundled tailer path stays valid.
- Requires macOS 14+.
