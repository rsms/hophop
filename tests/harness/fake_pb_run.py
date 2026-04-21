#!/usr/bin/env python3
from __future__ import annotations

import os
import sys


def fail(message: str) -> int:
    sys.stderr.write(f"{message}\n")
    return 1


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[1] != "run":
        return fail(f"unexpected argv: {sys.argv[1:]!r}")
    wasm_path = sys.argv[2]
    if not wasm_path.endswith(".wasm"):
        return fail(f"expected .wasm input, got {wasm_path!r}")
    if not os.path.isfile(wasm_path):
        return fail(f"missing wasm input: {wasm_path}")
    with open(wasm_path, "rb") as f:
        if f.read(4) != b"\0asm":
            return fail("input is not a wasm module")
    sys.stdout.write("fake pb run ok\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
