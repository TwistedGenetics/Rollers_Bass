#!/usr/bin/env bash
# Build and run the desktop DSP tests (no Korg toolchain or SDK needed).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
c++ -O2 -std=c++17 -Wall -Wextra -I core test/render.cc -o test/render
./test/render
