#!/bin/sh
set -eu

cd "$(dirname "$0")/../.."
python3 tools/test.py run --suite examples.rosetta --build-dir _build/macos-aarch64-debug --cc clang
