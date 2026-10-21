#!/usr/bin/env bash
# Build the Switch NRO with the devkitPro toolchain (via Docker).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/switch"

rm -rf build
docker run --rm \
  -v "$ROOT/switch:/src" \
  -w /src \
  devkitpro/devkita64:latest \
  make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

mkdir -p "$ROOT/dist"
cp -f "$ROOT/switch/osmo-remote.nro" "$ROOT/dist/osmo-remote.nro"
echo "built: $ROOT/dist/osmo-remote.nro"
ls -la "$ROOT/dist/osmo-remote.nro"
