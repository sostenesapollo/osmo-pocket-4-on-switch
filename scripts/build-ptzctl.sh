#!/usr/bin/env bash
# Compile the ptzctl helper (gimbal control over UVC, via IOKit).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/mac"
clang -O2 -o ptzctl ptzctl.c -framework CoreFoundation -framework IOKit
echo "built $ROOT/mac/ptzctl"
