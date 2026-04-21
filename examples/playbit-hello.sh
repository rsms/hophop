#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

PLAYBIT_APP=${PLAYBIT_APP:-$HOME/playbit/engine/_build/macos-aarch64-debug/Playbit.app}

set -x

_build/macos-aarch64-debug/slc \
    --platform playbit \
    genpkg:wasm \
    examples/console_log.sl \
    /tmp/sl-console_log.wasm

exec "$PLAYBIT_APP/Contents/MacOS/Playbit" /tmp/sl-console_log.wasm
