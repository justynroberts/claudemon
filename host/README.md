# claudemon-tailer

Watches `~/.claude/projects/*/*.jsonl` and POSTs token-usage deltas to a
claudemon device on the LAN. Per-project session logs are parsed for
`assistant.message.usage` records; one POST every `interval` seconds.

## Setup

1. On the device, hit the captive portal (`claudemon-xxxx` SSID, password
   `claudemon`) and copy the shared secret shown on the form.
2. Run the tailer once to drop a default config:
   ```bash
   /usr/bin/python3 claudemon-tailer.py
   ```
3. Edit `~/.config/claudemon/tailer.toml`:
   ```toml
   device_url    = "http://claudemon.local"
   shared_secret = "<value-from-portal>"
   interval      = 10
   ```
4. Start it:
   ```bash
   /usr/bin/python3 claudemon-tailer.py
   ```

## macOS 15 (Sequoia) — Local Network permission

macOS 15+ gates LAN access per-binary. The device lives on your local
network, so the interpreter running the tailer must be allowed to reach it.

- **Apple's `/usr/bin/python3` already has LAN access** — use it and the
  tailer just works. It's Python 3.9 (no `tomllib`), but the tailer ships a
  built-in config parser, so **no `pip install` is needed**.
- A **Homebrew/pyenv `python3` is blocked** from LAN devices until granted
  permission, and will fail every push with `OSError(65, 'No route to host')`
  even though `getaddrinfo` resolves and `curl` works. If you must use one,
  grant it access under **System Settings → Privacy & Security → Local
  Network** (toggle on your Terminal app), or just run `/usr/bin/python3`.

Quick check that your interpreter can reach the device:
```bash
/usr/bin/python3 -c "import urllib.request as u; print(u.urlopen('http://claudemon.local/status', timeout=5).read())"
```

## Grouping projects into envs

By default each project directory becomes its own env (named after the
last path segment of `cwd`). To collapse several projects into one logical
env, add `[groups]` to the config:

```toml
[groups]
work        = ["/Users/me/work/proj-a", "/Users/me/work/proj-b"]
experiments = ["/Users/me/sandbox"]
```

Anything not matched falls back to the directory-name default.

## Auto-start (macOS launchd)

Save as `~/Library/LaunchAgents/com.local.claudemon.tailer.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.local.claudemon.tailer</string>
  <key>ProgramArguments</key><array>
    <string>/usr/bin/python3</string>
    <string>/Users/CHANGEME/work/claudemon/host/claudemon-tailer.py</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/tmp/claudemon-tailer.log</string>
  <key>StandardErrorPath</key><string>/tmp/claudemon-tailer.err</string>
</dict></plist>
```

Then `launchctl load ~/Library/LaunchAgents/com.local.claudemon.tailer.plist`.

## State file

`~/.config/claudemon/tailer-state.json` tracks per-file byte offsets so
restarts don't double-count usage. On first run the offsets are set to
the current file sizes — only usage *after* the first launch is tracked.
Delete the state file to replay all history (the device aggregates by
addition, so this will double-count if you've already pushed).
