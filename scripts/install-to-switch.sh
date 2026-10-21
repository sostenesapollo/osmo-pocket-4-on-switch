#!/usr/bin/env bash
# Copy the NRO + host config to the Switch over DBI's MTP responder.
#
#   ./scripts/install-to-switch.sh          clean old builds, then install
#   ./scripts/install-to-switch.sh --clean  clean only
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="osmo-remote"

# OpenMTP holds the USB device exclusively; DBI needs it free.
osascript -e 'quit app "OpenMTP"' 2>/dev/null || true
sleep 1

if ! mtp-detect 2>&1 | grep -q 'Found 1 device'; then
  echo "Switch MTP not detected."
  echo "On the Switch: open DBI → 'Run MTP responder', then run this again."
  exit 1
fi

if [[ ! -x "$ROOT/mac/mtp_install" || "$ROOT/mac/mtp_install.c" -nt "$ROOT/mac/mtp_install" ]]; then
  cc -O2 -o "$ROOT/mac/mtp_install" "$ROOT/mac/mtp_install.c" \
    -I/opt/homebrew/include -L/opt/homebrew/lib -lmtp
fi

echo "== removing older builds =="
"$ROOT/mac/mtp_install" clean "$NAME"

if [[ "${1:-}" == "--clean" ]]; then
  echo "Done (clean only)."
  exit 0
fi

IP="$(ipconfig getifaddr en0 2>/dev/null || true)"
if [[ -n "$IP" ]]; then
  echo "${IP}:8765" > "$ROOT/dist/host.txt"
fi

if [[ ! -f "$ROOT/dist/osmo-remote.nro" ]]; then
  echo "NRO missing — run ./scripts/build-nro.sh first"
  exit 1
fi

echo "== installing =="
"$ROOT/mac/mtp_install" push \
  "$ROOT/dist/osmo-remote.nro" "$ROOT/dist/host.txt" "$NAME"

echo
echo "sdmc:/switch/${NAME}.nro"
echo "sdmc:/switch/${NAME}-host.txt"
echo "Leave MTP on the Switch → hbmenu → Osmo Remote → A"
