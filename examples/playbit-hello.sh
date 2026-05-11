#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

PLAYBIT_APP=${PLAYBIT_APP:-$HOME/playbit/engine/_build/macos-aarch64-debug/Playbit.app}

set -x

_build/macos-aarch64-debug/hop \
    --platform playbit \
    genpkg:wasm \
    examples/hello.hop \
    /tmp/hop-hello.wasm

exec "$PLAYBIT_APP/Contents/MacOS/Playbit" /tmp/hop-hello.wasm
