# Window Capture CLI Smoke Test

Minimal scripts to prove the Sunshine window-capture + Moonlight CLI workflow.

## Files

| File | Runs on | Purpose |
|---|---|---|
| `notepad-smoke.ps1` | Windows host (DEV-3) | Launch Notepad, find its HWND, inject Sunshine app entry, check status, stop |
| `remote-notepad-smoke.sh` | macOS client | SSH into host to run the PS1, then launch Moonlight stream |
| `apps-notepad-window.json` | Reference | The Sunshine app entry for Notepad window capture |

## Quick Start

### One command from Mac

```bash
./remote-notepad-smoke.sh
```

This runs the full cycle: sync repo to DEV-3, inject app entry, launch Notepad, start Moonlight stream.

### Step by step

```bash
# 1. Sync scripts to DEV-3 (pushes local, pulls on host)
./remote-notepad-smoke.sh sync

# 2. Inject the app entry into Sunshine's apps.json on DEV-3
./remote-notepad-smoke.sh inject

# 3. Launch Notepad on DEV-3
./remote-notepad-smoke.sh launch

# 4. Start the Moonlight stream
./remote-notepad-smoke.sh stream

# 5. When done, tear everything down
./remote-notepad-smoke.sh teardown
```

### Host-side only (on DEV-3 directly)

```powershell
# List all capturable windows
.\notepad-smoke.ps1 -Action list

# Launch Notepad and get its HWND
.\notepad-smoke.ps1 -Action launch

# Inject the app entry into Sunshine
.\notepad-smoke.ps1 -Action inject

# Check Notepad status
.\notepad-smoke.ps1 -Action status

# Stop Notepad
.\notepad-smoke.ps1 -Action stop
```

## Configuration

Edit the top of `remote-notepad-smoke.sh`:

| Variable | Default | Description |
|---|---|---|
| `DEV3_HOST` | `RUG-DEV-3` | SSH hostname for the Windows host |
| `DEV3_IP` | `192.168.4.23` | IP address Moonlight connects to |
| `SUNSHINE_APP_NAME` | `Notepad (Window CLI)` | Must match the name in Sunshine's app list |

Or override via environment:

```bash
DEV3_HOST=my-pc DEV3_IP=10.0.0.5 ./remote-notepad-smoke.sh
```

## What Success Looks Like

1. Moonlight opens a single window.
2. Only Notepad is visible (no desktop, taskbar, or other windows).
3. Cursor is visible (may need server-side cursor fix first).
4. Keyboard input works in Notepad.
5. Closing the Moonlight window or running `teardown` cleans up.

## Prerequisites

- SSH access from Mac to DEV-3 (`ssh RUG-DEV-3` works)
- PowerShell available on DEV-3
- Sunshine built and running on DEV-3 with window-capture support
- Moonlight installed on Mac
- DEV-3 already paired with Moonlight
