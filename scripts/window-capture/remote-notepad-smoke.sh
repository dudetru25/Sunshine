#!/usr/bin/env bash
# -----------------------------------------------------------------------
# Mac-side smoke test for Sunshine window-capture CLI workflow.
#
# Usage:
#   ./remote-notepad-smoke.sh              # full cycle: sync + inject + launch + stream
#   ./remote-notepad-smoke.sh sync         # push local changes, pull on DEV-3
#   ./remote-notepad-smoke.sh launch       # just launch Notepad on host
#   ./remote-notepad-smoke.sh inject       # just inject the Sunshine app entry
#   ./remote-notepad-smoke.sh status       # check if Notepad is running on host
#   ./remote-notepad-smoke.sh stream       # start Moonlight stream (assumes app exists)
#   ./remote-notepad-smoke.sh list         # list capturable windows on host
#   ./remote-notepad-smoke.sh stop         # stop Notepad on host
#   ./remote-notepad-smoke.sh teardown     # stop Notepad + kill local Moonlight
# -----------------------------------------------------------------------

set -uo pipefail

# ---- Configuration (edit these for your setup) ----

DEV3_HOST="${DEV3_HOST:-RUG-DEV-3}"
DEV3_IP="${DEV3_IP:-192.168.4.23}"
SUNSHINE_APP_NAME="Notepad (Window CLI)"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEV3_REPO="C:\\Dev\\Sunshine"
DEV3_REPO_POSIX="/c/Dev/Sunshine"

HOST_SCRIPT_REL="scripts/window-capture/notepad-smoke.ps1"
HOST_SCRIPT="${DEV3_REPO}\\${HOST_SCRIPT_REL//\//\\}"

# Moonlight binary path on macOS
if [[ -d "/Applications/Moonlight.app" ]]; then
    MOONLIGHT_BIN="/Applications/Moonlight.app/Contents/MacOS/Moonlight"
else
    MOONLIGHT_BIN="moonlight"
fi

# ---- Helpers ----

log() { echo "[smoke] $*"; }
err() { echo "[smoke] ERROR: $*" >&2; }

bail() {
    err "$1"
    exit 1
}

run_host() {
    local action="$1"
    log "Running on ${DEV3_HOST}: notepad-smoke.ps1 -Action $action"
    local output
    output=$(ssh "${DEV3_HOST}" "powershell.exe -ExecutionPolicy Bypass -File \"${HOST_SCRIPT}\" -Action $action" 2>&1) || {
        err "Host command failed (action=$action):"
        echo "$output" >&2
        return 1
    }
    echo "$output"
}

check_ssh() {
    log "Checking SSH connectivity to ${DEV3_HOST}..."
    if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "${DEV3_HOST}" "echo ok" &>/dev/null; then
        bail "Cannot SSH to ${DEV3_HOST}. Check hostname, network, and SSH config."
    fi
    log "SSH OK."
}

check_script_exists() {
    log "Checking if PS1 script exists on ${DEV3_HOST}..."
    if ! ssh "${DEV3_HOST}" "powershell.exe -Command \"Test-Path '${HOST_SCRIPT}'\"" 2>/dev/null | grep -qi "true"; then
        err "Script not found on host: ${HOST_SCRIPT}"
        err "Run: ./remote-notepad-smoke.sh sync"
        return 1
    fi
    log "Host script found."
}

# ---- Actions ----

do_sync() {
    log "Syncing repo to ${DEV3_HOST}..."

    log "Pushing local changes..."
    cd "$REPO_ROOT"
    git add -A
    git diff --cached --quiet && log "Nothing new to commit." || {
        git commit -m "sync: window-capture smoke test scripts"
        log "Committed local changes."
    }
    git push || bail "Git push failed."
    log "Pushed to origin."

    log "Pulling on ${DEV3_HOST}..."
    ssh "${DEV3_HOST}" "cd ${DEV3_REPO_POSIX} && git pull" 2>&1 || {
        log "POSIX-style pull failed, trying PowerShell..."
        ssh "${DEV3_HOST}" "powershell.exe -Command \"cd '${DEV3_REPO}'; git pull\"" 2>&1 || bail "Git pull on host failed."
    }
    log "Sync complete."
}

do_inject() {
    log "Injecting Notepad app entry into Sunshine apps.json on ${DEV3_HOST}..."
    run_host inject || bail "Inject failed. Run 'sync' first?"
}

do_launch() {
    log "Launching Notepad on ${DEV3_HOST}..."
    run_host launch || bail "Launch failed."
}

do_status() {
    run_host status || bail "Status check failed."
}

do_list() {
    run_host list || bail "List failed."
}

do_stop() {
    log "Stopping Notepad on ${DEV3_HOST}..."
    run_host stop || bail "Stop failed."
}

do_stream() {
    log "Starting Moonlight stream to ${DEV3_IP} for '${SUNSHINE_APP_NAME}'..."

    if ! command -v "$MOONLIGHT_BIN" &>/dev/null && [[ ! -x "$MOONLIGHT_BIN" ]]; then
        bail "Moonlight not found at $MOONLIGHT_BIN. Install Moonlight or set MOONLIGHT_BIN."
    fi

    # Window-capture streams default to absolute mouse + windowed mode.
    # These are NOT shortcuts the user should need -- they're the natural UX.
    # Stock Moonlight emergency overrides if something goes wrong:
    #   Ctrl+Alt+Shift+M  toggle absolute/relative mouse
    #   Ctrl+Alt+Shift+C  toggle cursor visibility
    "$MOONLIGHT_BIN" stream "$DEV3_IP" "$SUNSHINE_APP_NAME" \
        --absolute-mouse \
        --display-mode windowed \
        --quit-after &
    MOONLIGHT_PID=$!
    log "Moonlight started (PID: $MOONLIGHT_PID)"
    log "Stream running. Press Ctrl+C to disconnect."
    wait "$MOONLIGHT_PID" 2>/dev/null || true
}

do_teardown() {
    do_stop || true
    log "Killing local Moonlight processes..."
    pkill -f "Moonlight" 2>/dev/null || true
    log "Teardown complete."
}

do_full() {
    log "=== Full smoke test: sync -> inject -> stream ==="
    log "(Sunshine launches the app when Moonlight connects -- no pre-launch needed)"
    echo ""

    check_ssh
    echo ""

    do_sync
    echo ""

    check_script_exists || bail "Sync did not place the script on host. Check DEV-3 repo path."
    echo ""

    do_inject
    echo ""

    log "Restarting SunshineService so it picks up the new app entry..."
    ssh "${DEV3_HOST}" "net stop SunshineService 2>nul & net start SunshineService" 2>&1 || {
        log "Service restart may need admin. Trying PowerShell..."
        ssh "${DEV3_HOST}" "powershell.exe -Command \"Restart-Service SunshineService -Force\"" 2>&1 || true
    }
    log "Waiting 3s for Sunshine to start..."
    sleep 3

    do_stream
}

# ---- Main ----

ACTION="${1:-full}"

case "$ACTION" in
    full)     do_full     ;;
    sync)     do_sync     ;;
    inject)   do_inject   ;;
    launch)   do_launch   ;;
    status)   do_status   ;;
    stream)   do_stream   ;;
    list)     do_list     ;;
    stop)     do_stop     ;;
    teardown) do_teardown ;;
    *)
        err "Unknown action: $ACTION"
        echo "Usage: $0 [full|sync|inject|launch|status|stream|list|stop|teardown]"
        exit 1
        ;;
esac
