#!/usr/bin/env bash
#
# Launch sy01b-server, hardened against the recurring bench failure where
# ModemManager grabs the CH340 (1a86:7523) USB-serial dongle, probes it
# with AT commands and resets it, causing endless USB re-enumeration and a
# pump that never replies. Kill ModemManager before opening the port.
#
# Port identity is NOT handled here: server/pump.toml sets the pump port to
# the USB VID:PID ("1A86:7523"), which the driver resolves to a concrete
# /dev/ttyUSB* at runtime (see _resolve_port). That survives a renumber or
# a different USB socket without this script rewriting the config.
#
set -euo pipefail
cd "$(dirname "$0")"

CONFIG="server/pump.toml"

# 1. Kill ModemManager if present (no-op if absent or unprivileged).
if pgrep -x ModemManager >/dev/null 2>&1; then
    echo "server_run: killing ModemManager (it grabs the CH340)"
    pkill -9 -x ModemManager 2>/dev/null || true
    sleep 1
fi

# 2. Stop any prior sy01b-server so re-running this script always reclaims
#    the HTTP port. `ss` can't see the listener/pid inside this container,
#    so match the process instead. The [s] bracket keeps grep from matching
#    its own command line. `|| true`: grep exits 1 when there is no prior
#    server, which would abort the script under `set -euo pipefail`.
OLD=$(ps -eo pid,cmd 2>/dev/null | grep '[s]y01b-server' | awk '{print $1}' || true)
if [ -n "$OLD" ]; then
    echo "server_run: stopping prior server (pid $(echo $OLD | tr '\n' ' '))"
    kill $OLD 2>/dev/null || true
    sleep 1
fi

exec .venv/bin/sy01b-server --config "$CONFIG"

# http://localhost:17050/docs
