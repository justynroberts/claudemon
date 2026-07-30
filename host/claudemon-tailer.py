#!/usr/bin/env python3
"""
claudemon-tailer — watch ~/.claude/projects/*/*.jsonl for token usage
and POST aggregates to a claudemon device on the LAN.

Run on the same host (or any host) where Claude Code stores its session
logs. Pushes a batch to the device every --interval seconds.

Config: ~/.config/claudemon/tailer.toml  (created on first run)

  device_url    = "http://claudemon.local"
  shared_secret = "..."                # shown on the device portal
  interval      = 10                   # seconds between pushes

  # Optional: collapse multiple project cwds into one logical "env".
  # Anything not listed falls into an env named after the last path segment.
  [groups]
  work       = ["/Users/me/work/proj-a", "/Users/me/work/proj-b"]
  experiments = ["/Users/me/sandbox"]
"""
from __future__ import annotations

import json
import os
import sys
import time
import urllib.request
import urllib.error
from collections import defaultdict
from datetime import datetime
from pathlib import Path

# Usage-window sizes and default budgets (tokens) for the session/week gauges.
# These are a LOCAL PROXY for Claude Code's /usage meter — the real per-plan
# limit isn't stored on disk, so the percentage is measured against a budget you
# set in tailer.toml. Tune session_budget / week_budget to match your plan.
SESSION_SECS = 5 * 3600
WEEK_SECS    = 7 * 86400
MONTH_SECS   = 30 * 86400
DEFAULT_SESSION_BUDGET = 200_000_000
DEFAULT_WEEK_BUDGET    = 2_000_000_000
DEFAULT_MONTH_BUDGET   = 6_000_000_000

try:
    import tomllib   # Python 3.11+
except ImportError:
    try:
        import tomli as tomllib
    except ImportError:
        tomllib = None


def _parse_simple_toml(text: str) -> dict:
    """Minimal TOML reader for this config's shape — so the tailer runs under
    the Apple-signed /usr/bin/python3 (3.9, no tomllib) without `pip install`.
    On macOS 15+ that interpreter is the one with Local Network access; a
    Homebrew python is blocked from LAN devices until granted permission.

    Handles: top-level key = "string" | int, a [groups] table whose values are
    arrays of strings, and # comments. Not a general TOML parser.
    """
    out: dict = {}
    cur = out
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1].strip()
            cur = out.setdefault(name, {})
            continue
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key, val = key.strip(), val.strip()
        if val.startswith("[") and val.endswith("]"):
            items = [v.strip().strip('"').strip("'")
                     for v in val[1:-1].split(",") if v.strip()]
            cur[key] = items
        elif val and val[0] in "\"'":
            cur[key] = val.strip('"').strip("'")
        else:
            try:
                cur[key] = int(val)
            except ValueError:
                cur[key] = val.strip('"').strip("'")
    return out

CFG_PATH    = Path.home() / ".config" / "claudemon" / "tailer.toml"
STATE_PATH  = Path.home() / ".config" / "claudemon" / "tailer-state.json"
LOG_ROOT    = Path.home() / ".claude" / "projects"


def load_cfg() -> dict:
    if not CFG_PATH.exists():
        CFG_PATH.parent.mkdir(parents=True, exist_ok=True)
        CFG_PATH.write_text(
            'device_url    = "http://claudemon.local"\n'
            'shared_secret = "REPLACE-WITH-VALUE-FROM-DEVICE-PORTAL"\n'
            'interval      = 10\n'
            '\n'
            '# Session (5h) / week (7d) / month (30d) gauges are a local proxy for\n'
            '# /usage. Tune these token budgets so the percentages match your plan.\n'
            'session_budget = 200000000\n'
            'week_budget    = 2000000000\n'
            'month_budget   = 6000000000\n'
            '\n'
            '# [groups]\n'
            '# work = ["/Users/me/work/proj-a"]\n'
        )
        print(f"wrote default config at {CFG_PATH} — edit and re-run", file=sys.stderr)
        sys.exit(1)
    if tomllib is not None:
        with CFG_PATH.open("rb") as f:
            return tomllib.load(f)
    # No tomllib/tomli (e.g. macOS system python 3.9) — use the built-in
    # minimal parser so no pip install is needed.
    return _parse_simple_toml(CFG_PATH.read_text())


def load_state() -> dict:
    if not STATE_PATH.exists():
        return {}
    try:
        return json.loads(STATE_PATH.read_text())
    except Exception:
        return {}


def save_state(state: dict) -> None:
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    STATE_PATH.write_text(json.dumps(state))


def env_for_cwd(cwd: str, groups: dict[str, list[str]]) -> str:
    for label, paths in groups.items():
        for p in paths:
            if cwd == p or cwd.startswith(p.rstrip("/") + "/"):
                return label
    return Path(cwd).name or "default"


def scan(state: dict, groups: dict) -> dict:
    """Return {env: {model: {input, output, cache_create, cache_read}}}."""
    deltas: dict = defaultdict(lambda: defaultdict(lambda: {
        "input": 0, "output": 0, "cache_create": 0, "cache_read": 0,
    }))
    if not LOG_ROOT.exists():
        return deltas

    for jsonl in LOG_ROOT.rglob("*.jsonl"):
        key = str(jsonl)
        try:
            size = jsonl.stat().st_size
        except FileNotFoundError:
            continue
        offset = state.get(key, 0)
        # Log rotation safety: if file shrunk, reset offset.
        if offset > size:
            offset = 0
        if offset == size:
            continue
        try:
            with jsonl.open("rb") as f:
                f.seek(offset)
                buf = f.read()
        except OSError:
            continue

        new_offset = offset
        # Parse complete lines only; track byte offset.
        consumed = 0
        for raw in buf.splitlines(keepends=True):
            if not raw.endswith(b"\n"):
                break  # partial line — leave for next pass
            consumed += len(raw)
            try:
                obj = json.loads(raw)
            except Exception:
                continue
            if obj.get("type") != "assistant":
                continue
            msg = obj.get("message") or {}
            usage = msg.get("usage") or {}
            if not usage:
                continue
            cwd   = obj.get("cwd") or ""
            model = msg.get("model") or "unknown"
            env   = env_for_cwd(cwd, groups) if cwd else "default"
            d = deltas[env][model]
            d["input"]        += int(usage.get("input_tokens") or 0)
            d["output"]       += int(usage.get("output_tokens") or 0)
            d["cache_create"] += int(usage.get("cache_creation_input_tokens") or 0)
            d["cache_read"]   += int(usage.get("cache_read_input_tokens") or 0)
        new_offset = offset + consumed
        state[key] = new_offset
    return deltas


def push(cfg: dict, deltas: dict) -> bool:
    if not deltas:
        return True
    payload = []
    for env, models in deltas.items():
        for model, c in models.items():
            if not any(c.values()):
                continue
            payload.append({"env": env, "model": model, **c})
    if not payload:
        return True

    body = json.dumps(payload).encode()
    url = cfg["device_url"].rstrip("/") + "/ingest"
    req = urllib.request.Request(
        url, data=body, method="POST",
        headers={
            "Content-Type":  "application/json",
            "Authorization": "Bearer " + cfg.get("shared_secret", ""),
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=8) as resp:
            resp.read()
            return True
    except urllib.error.HTTPError as e:
        print(f"device returned {e.code}: {e.reason}", file=sys.stderr)
        return False
    except Exception as e:
        print(f"push failed: {e}", file=sys.stderr)
        return False


def _parse_ts(s: str):
    """ISO-8601 (e.g. 2026-05-10T16:33:37.899Z) -> epoch seconds, or None."""
    if not s:
        return None
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except Exception:
        return None


class UsageWindow:
    """Rolling record of (epoch, tokens) over the last 7 days, read incrementally
    from the same jsonl logs. Its own byte offsets keep it from double-counting
    (independent of the delta scanner's offsets)."""

    def __init__(self):
        self.events: list[tuple[float, int]] = []
        self.offsets: dict[str, int] = {}

    def refresh(self) -> None:
        if not LOG_ROOT.exists():
            return
        cutoff = time.time() - MONTH_SECS
        for jsonl in LOG_ROOT.rglob("*.jsonl"):
            key = str(jsonl)
            try:
                st = jsonl.stat()
            except OSError:
                continue
            # Files untouched for >7d and never seen have nothing in-window.
            if st.st_mtime < cutoff and key not in self.offsets:
                self.offsets[key] = st.st_size
                continue
            off = self.offsets.get(key, 0)
            if off > st.st_size:
                off = 0
            if off == st.st_size:
                continue
            try:
                with jsonl.open("rb") as f:
                    f.seek(off)
                    buf = f.read()
            except OSError:
                continue
            consumed = 0
            for raw in buf.splitlines(keepends=True):
                if not raw.endswith(b"\n"):
                    break
                consumed += len(raw)
                try:
                    o = json.loads(raw)
                except Exception:
                    continue
                if o.get("type") != "assistant":
                    continue
                u = (o.get("message") or {}).get("usage") or {}
                if not u:
                    continue
                ep = _parse_ts(o.get("timestamp"))
                if ep is None:
                    continue
                tok = (int(u.get("input_tokens") or 0)
                       + int(u.get("output_tokens") or 0)
                       + int(u.get("cache_creation_input_tokens") or 0)
                       + int(u.get("cache_read_input_tokens") or 0))
                self.events.append((ep, tok))
            self.offsets[key] = off + consumed

    def sums(self) -> tuple[int, int, int]:
        now = time.time()
        mo_cut, wk_cut, se_cut = now - MONTH_SECS, now - WEEK_SECS, now - SESSION_SECS
        self.events = [e for e in self.events if e[0] >= mo_cut]
        session = sum(k for t, k in self.events if t >= se_cut)
        week = sum(k for t, k in self.events if t >= wk_cut)
        month = sum(k for t, k in self.events)
        return session, week, month


def push_usage(cfg: dict, uw: UsageWindow) -> bool:
    uw.refresh()
    session, week, month = uw.sums()
    sb = int(cfg.get("session_budget", DEFAULT_SESSION_BUDGET) or 0)
    wb = int(cfg.get("week_budget", DEFAULT_WEEK_BUDGET) or 0)
    mb = int(cfg.get("month_budget", DEFAULT_MONTH_BUDGET) or 0)
    payload = {
        "session_tokens": session, "week_tokens": week, "month_tokens": month,
        "session_budget": sb, "week_budget": wb, "month_budget": mb,
        "session_pct": int(session * 100 / sb) if sb > 0 else 0,
        "week_pct": int(week * 100 / wb) if wb > 0 else 0,
        "month_pct": int(month * 100 / mb) if mb > 0 else 0,
    }
    body = json.dumps(payload).encode()
    url = cfg["device_url"].rstrip("/") + "/usage"
    req = urllib.request.Request(
        url, data=body, method="POST",
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer " + cfg.get("shared_secret", "")},
    )
    try:
        with urllib.request.urlopen(req, timeout=8) as resp:
            resp.read()
            return True
    except Exception as e:
        print(f"usage push failed: {e}", file=sys.stderr)
        return False


def main() -> None:
    cfg = load_cfg()
    interval = int(cfg.get("interval", 10))
    groups = cfg.get("groups", {})
    state = load_state()
    usage = UsageWindow()

    print(f"claudemon-tailer → {cfg['device_url']}  (every {interval}s)")
    # Prime: on first run, mark all existing bytes as already-seen so we
    # only track usage from this moment forward.
    if not state:
        for jsonl in LOG_ROOT.rglob("*.jsonl"):
            try:
                state[str(jsonl)] = jsonl.stat().st_size
            except FileNotFoundError:
                pass
        save_state(state)
        print(f"primed at {len(state)} files; tracking forward only")

    while True:
        try:
            deltas = scan(state, groups)
            if push(cfg, deltas):
                save_state(state)
            push_usage(cfg, usage)   # session/week gauges (absolute, windowed)
        except KeyboardInterrupt:
            return
        except Exception as e:
            print(f"loop error: {e}", file=sys.stderr)
        time.sleep(interval)


def backfill_once() -> bool:
    """One-shot: push ALL historical per-project usage + the session/week/month
    gauges, then exit. Used by the menu bar app's 'Backfill history' button so a
    freshly-booted device (RAM store) fills instantly instead of only tracking
    forward. Uses a throwaway state so it never touches the live tailer's offsets.
    """
    cfg = load_cfg()
    groups = cfg.get("groups", {})
    uw = UsageWindow()
    for attempt in range(4):
        try:
            deltas = scan({}, groups)          # empty state -> full history
            if push(cfg, deltas) and push_usage(cfg, uw):
                print("backfill: ok")
                return True
        except Exception as e:
            print(f"backfill error: {e}", file=sys.stderr)
        time.sleep(2)
    print("backfill: failed", file=sys.stderr)
    return False


if __name__ == "__main__":
    if "--backfill" in sys.argv:
        sys.exit(0 if backfill_once() else 1)
    main()
