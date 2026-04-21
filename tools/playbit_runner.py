#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path


DEFAULT_PLAYBIT_BIN = (
    Path.home()
    / "playbit/engine/_build/macos-aarch64-debug/Playbit.app/Contents/MacOS/Playbit"
)
TIMESTAMP_RE = re.compile(r"^[A-Z][a-z]{2} [ 0-9][0-9]? \d\d:\d\d:\d\d\.\d{6} ")


def fail(message: str) -> int:
    sys.stderr.write(f"{message}\n")
    return 1


def resolve_playbit_bin() -> Path:
    for env_name in ("SL_PLAYBIT_BIN", "PLAYBIT_BIN"):
        value = os.environ.get(env_name)
        if value:
            return Path(value)
    return DEFAULT_PLAYBIT_BIN


def route_stderr(stderr_text: str) -> tuple[str, str]:
    stdout_parts: list[str] = []
    stderr_parts: list[str] = []
    for chunk in stderr_text.splitlines(keepends=True):
        match = TIMESTAMP_RE.match(chunk)
        if match is None:
            stderr_parts.append(chunk)
            continue
        payload = chunk[match.end() :]
        if payload.startswith("panic:"):
            if payload and not payload.endswith("\n"):
                payload += "\n"
            stderr_parts.append(payload)
        else:
            stdout_parts.append(payload)
    return "".join(stdout_parts), "".join(stderr_parts)


def main() -> int:
    if len(sys.argv) != 2:
        return fail("usage: tools/playbit_runner.py <module.wasm>")

    wasm_path = Path(sys.argv[1])
    if not wasm_path.is_file():
        return fail(f"playbit: missing wasm module: {wasm_path}")

    playbit_bin = resolve_playbit_bin()
    if not playbit_bin.is_file():
        return fail(
            "playbit: missing Playbit app; set SL_PLAYBIT_BIN or build "
            f"{DEFAULT_PLAYBIT_BIN}"
        )

    env = os.environ.copy()
    env["MallocNanoZone"] = "0"

    cp = subprocess.run(
        [str(playbit_bin), "--remote-control", str(wasm_path)],
        input="",
        text=True,
        capture_output=True,
        env=env,
    )

    routed_stdout, routed_stderr = route_stderr(cp.stderr)
    if routed_stdout:
        sys.stdout.write(routed_stdout)
    if cp.stdout:
        routed_stderr += (
            "playbit: unexpected remote-control stdout\n"
            f"{cp.stdout}"
            f"{'' if cp.stdout.endswith(chr(10)) else chr(10)}"
        )
    if routed_stderr:
        sys.stderr.write(routed_stderr)
    return cp.returncode


if __name__ == "__main__":
    raise SystemExit(main())
