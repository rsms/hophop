#!/usr/bin/env python3
from __future__ import annotations

import argparse
import difflib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tests" / "tests.jsonl"
ARENA_GROW_TEST_C_PATH = ROOT / "tests" / "harness" / "arena_grow_test.c"
TEST_ROOT_IGNORED_NAMES = {"tests.jsonl", "README.md", "harness", ".DS_Store"}
TEST_ROOT_TRACKED_SUFFIXES = {".sl", ".stderr", ".ast", ".tokens"}


@dataclass
class TestCase:
    index: int
    data: Dict[str, Any]

    @property
    def id(self) -> str:
        return str(self.data["id"])

    @property
    def suite(self) -> str:
        return str(self.data["suite"])

    @property
    def kind(self) -> str:
        return str(self.data["kind"])


@dataclass
class RunResult:
    case: TestCase
    ok: bool
    duration: float
    detail: str


@dataclass
class RunContext:
    build_dir: Path
    slc: Path
    cc: str
    update: bool
    sidecar_codegen: bool


def fail(msg: str) -> tuple[bool, str]:
    return False, msg


def ok(msg: str = "") -> tuple[bool, str]:
    return True, msg


def abs_path(path: str) -> Path:
    p = Path(path)
    if p.is_absolute():
        return p
    return ROOT / p


def read_text(path: str) -> str:
    return abs_path(path).read_text()


def run_cmd(args: List[str], cwd: Optional[Path] = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=str(cwd or ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def sanitize_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)


def slc_args(ctx: RunContext, mode: str, input_path: str) -> List[str]:
    if mode in ("", "_"):
        return [str(ctx.slc), input_path]
    return [str(ctx.slc), mode, input_path]


def check_text_constraints(text: str, case: Dict[str, Any]) -> tuple[bool, str]:
    contains = case.get("contains", [])
    for s in contains:
        if s not in text:
            return fail(f"missing expected text: {s!r}")

    not_contains = case.get("not_contains", [])
    for s in not_contains:
        if s in text:
            return fail(f"unexpected text present: {s!r}")

    regex_count = case.get("regex_count", [])
    for item in regex_count:
        pattern = item["pattern"]
        expected = int(item["count"])
        actual = len(re.findall(pattern, text, re.MULTILINE))
        if actual != expected:
            return fail(
                f"regex_count mismatch for pattern {pattern!r}: expected {expected}, got {actual}"
            )

    return ok()


def maybe_check_codegen_sidecar(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    if not ctx.sidecar_codegen:
        return ok()
    input_path = case.get("input")
    if not isinstance(input_path, str) or not input_path.endswith(".sl"):
        return ok()

    expected_path = abs_path(str(Path(input_path).with_suffix(".expected.c")))
    if not expected_path.exists():
        return ok()

    cp = run_cmd([str(ctx.slc), "genpkg:c", input_path])
    if cp.returncode != 0:
        return fail(
            "sidecar codegen check failed to run genpkg:c:\n"
            f"stderr:\n{cp.stderr}"
        )

    actual = cp.stdout
    expected = expected_path.read_text()
    if actual == expected:
        return ok()

    if ctx.update:
        expected_path.write_text(actual)
        return ok(f"updated {expected_path.relative_to(ROOT)}")

    diff = "".join(
        difflib.unified_diff(
            expected.splitlines(True),
            actual.splitlines(True),
            fromfile=str(expected_path.relative_to(ROOT)),
            tofile=f"{expected_path.relative_to(ROOT)}.actual",
        )
    )
    return fail(f"codegen sidecar mismatch:\n{diff}")


def kind_slc_stdout_eq(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    cp = run_cmd(slc_args(ctx, str(case["mode"]), str(case["input"])))
    if cp.returncode != 0:
        return fail(f"unexpected failure (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if cp.stderr:
        return fail(f"unexpected stderr:\n{cp.stderr}")
    expected = read_text(str(case["expect"]))
    if cp.stdout != expected:
        diff = "".join(
            difflib.unified_diff(
                expected.splitlines(True),
                cp.stdout.splitlines(True),
                fromfile=str(case["expect"]),
                tofile="actual.stdout",
            )
        )
        return fail(f"stdout mismatch:\n{diff}")
    return ok()


def kind_slc_ok(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    cp = run_cmd(slc_args(ctx, str(case["mode"]), str(case["input"])))
    if cp.returncode != 0:
        return fail(f"unexpected failure (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    if cp.stderr:
        return fail(f"unexpected stderr:\n{cp.stderr}")
    return ok()


def kind_slc_fail_stderr(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    cp = run_cmd(slc_args(ctx, str(case["mode"]), str(case["input"])))
    if cp.returncode == 0:
        return fail("expected failure but command succeeded")
    if cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    expected = read_text(str(case["expect_stderr"]))
    if cp.stderr != expected:
        diff = "".join(
            difflib.unified_diff(
                expected.splitlines(True),
                cp.stderr.splitlines(True),
                fromfile=str(case["expect_stderr"]),
                tofile="actual.stderr",
            )
        )
        return fail(f"stderr mismatch:\n{diff}")
    return ok()


def kind_slc_ok_stderr(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    cp = run_cmd(slc_args(ctx, str(case["mode"]), str(case["input"])))
    if cp.returncode != 0:
        return fail(f"unexpected failure (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    expected = read_text(str(case["expect_stderr"]))
    if cp.stderr != expected:
        diff = "".join(
            difflib.unified_diff(
                expected.splitlines(True),
                cp.stderr.splitlines(True),
                fromfile=str(case["expect_stderr"]),
                tofile="actual.stderr",
            )
        )
        return fail(f"stderr mismatch:\n{diff}")
    return ok()


def kind_slc_fail_no_stdout(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    cp = run_cmd(slc_args(ctx, str(case["mode"]), str(case["input"])))
    if cp.returncode == 0:
        return fail("expected failure but command succeeded")
    if cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    return ok()


def run_compile(ctx: RunContext, input_path: str, output_path: Path) -> subprocess.CompletedProcess[str]:
    return run_cmd([str(ctx.slc), "compile", input_path, "-o", str(output_path)])


def kind_compile_only(ctx: RunContext, case: Dict[str, Any], work_dir: Path) -> tuple[bool, str]:
    exe = work_dir / "program"
    cp = run_compile(ctx, str(case["input"]), exe)
    if cp.returncode != 0:
        return fail(f"compile failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if case.get("compile_stdout_empty", False) and cp.stdout:
        return fail(f"unexpected compile stdout:\n{cp.stdout}")
    if case.get("compile_stderr_empty", False) and cp.stderr:
        return fail(f"unexpected compile stderr:\n{cp.stderr}")
    if not exe.exists() or not os.access(exe, os.X_OK):
        return fail("compile output executable missing")
    return ok()


def kind_compile_and_run(ctx: RunContext, case: Dict[str, Any], work_dir: Path) -> tuple[bool, str]:
    exe = work_dir / "program"
    cp = run_compile(ctx, str(case["input"]), exe)
    if cp.returncode != 0:
        return fail(f"compile failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if case.get("compile_stdout_empty", False) and cp.stdout:
        return fail(f"unexpected compile stdout:\n{cp.stdout}")
    if case.get("compile_stderr_empty", False) and cp.stderr:
        return fail(f"unexpected compile stderr:\n{cp.stderr}")
    if not exe.exists() or not os.access(exe, os.X_OK):
        return fail("compile output executable missing")

    run_cp = run_cmd([str(exe)], cwd=work_dir)
    expect_nonzero = bool(case.get("expect_nonzero", False))
    expect_exit = int(case.get("expect_exit", 0))
    if expect_nonzero:
        if run_cp.returncode == 0:
            return fail("expected non-zero runtime exit code")
    elif run_cp.returncode != expect_exit:
        return fail(
            f"unexpected runtime exit code: expected {expect_exit}, got {run_cp.returncode}"
        )

    if case.get("run_stdout_empty", True) and run_cp.stdout:
        return fail(f"unexpected runtime stdout:\n{run_cp.stdout}")
    if case.get("run_stderr_empty", True) and run_cp.stderr:
        return fail(f"unexpected runtime stderr:\n{run_cp.stderr}")
    stderr_contains = case.get("stderr_contains")
    if stderr_contains is not None and str(stderr_contains) not in run_cp.stderr:
        return fail(
            f"runtime stderr does not contain expected text {stderr_contains!r}\n"
            f"actual stderr:\n{run_cp.stderr}"
        )

    return ok()


def kind_slc_run(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    cp = run_cmd([str(ctx.slc), "run", str(case["input"])])
    expect_nonzero = bool(case.get("expect_nonzero", False))
    expect_exit = int(case.get("expect_exit", 0))
    if expect_nonzero:
        if cp.returncode == 0:
            return fail("expected non-zero exit code")
    elif cp.returncode != expect_exit:
        return fail(f"unexpected exit code: expected {expect_exit}, got {cp.returncode}")

    if case.get("stdout_empty", True) and cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    if case.get("stderr_empty", True) and cp.stderr:
        return fail(f"unexpected stderr:\n{cp.stderr}")
    stderr_contains = case.get("stderr_contains")
    if stderr_contains is not None and str(stderr_contains) not in cp.stderr:
        return fail(f"stderr missing expected text {stderr_contains!r}\n{cp.stderr}")

    return ok()


def run_genpkg_mode(ctx: RunContext, mode: str, input_path: str) -> subprocess.CompletedProcess[str]:
    return run_cmd([str(ctx.slc), mode, input_path])


def compile_harness(ctx: RunContext, work_dir: Path, header_path: Path, harness_relpath: str) -> tuple[bool, str]:
    template_path = abs_path(harness_relpath)
    template = template_path.read_text()
    rendered = template.replace("@HEADER@", str(header_path))
    c_path = work_dir / "harness.c"
    obj_path = work_dir / "harness.o"
    c_path.write_text(rendered)

    args = [
        ctx.cc,
        "-std=c11",
        "-isystem",
        str(ctx.build_dir / "lib"),
        "-Wall",
        "-Wextra",
        "-Werror",
        "-c",
        str(c_path),
        "-o",
        str(obj_path),
    ]
    cp = run_cmd(args, cwd=work_dir)
    if cp.returncode != 0:
        return fail(f"harness compile failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    return ok()


def kind_genpkg_text_check(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    mode = str(case.get("mode", "genpkg:c"))
    cp = run_genpkg_mode(ctx, mode, str(case["input"]))
    if cp.returncode != 0:
        return fail(f"{mode} failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if case.get("stderr_empty", True) and cp.stderr:
        return fail(f"unexpected stderr:\n{cp.stderr}")
    return check_text_constraints(cp.stdout, case)


def kind_genpkg_compile(ctx: RunContext, case: Dict[str, Any], work_dir: Path) -> tuple[bool, str]:
    mode = str(case.get("mode", "genpkg:c"))
    cp = run_genpkg_mode(ctx, mode, str(case["input"]))
    if cp.returncode != 0:
        return fail(f"{mode} failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if case.get("stderr_empty", True) and cp.stderr:
        return fail(f"unexpected stderr:\n{cp.stderr}")

    ok_text, detail = check_text_constraints(cp.stdout, case)
    if not ok_text:
        return False, detail

    header_path = work_dir / "generated.h"
    header_path.write_text(cp.stdout)

    harness = case.get("harness")
    if isinstance(harness, str) and harness:
        return compile_harness(ctx, work_dir, header_path, harness)

    return ok()


def kind_libsl_freestanding(ctx: RunContext, work_dir: Path) -> tuple[bool, str]:
    src_header = ctx.build_dir / "libsl.h"
    if not src_header.exists():
        return fail(f"missing generated header: {src_header}")

    dst_header = work_dir / "libsl.h"
    shutil.copy2(src_header, dst_header)
    libsl_c = work_dir / "libsl.c"
    libsl_c.write_text('#define SL_IMPLEMENTATION\n#include "libsl.h"\n')

    host_args = [
        ctx.cc,
        "-std=c11",
        "-ffreestanding",
        "-nostdlib",
        "-fno-builtin",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-c",
        str(libsl_c),
        "-o",
        str(work_dir / "libsl.freestanding.o"),
    ]
    cp = run_cmd(host_args, cwd=work_dir)
    if cp.returncode != 0:
        return fail(f"freestanding compile failed\nstderr:\n{cp.stderr}")

    wasm_args = [
        ctx.cc,
        "-target",
        "wasm32-unknown-unknown",
        "-std=c11",
        "-ffreestanding",
        "-nostdlib",
        "-fno-builtin",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-c",
        str(libsl_c),
        "-o",
        str(work_dir / "libsl.wasm"),
    ]
    cp = run_cmd(wasm_args, cwd=work_dir)
    if cp.returncode != 0:
        return fail(f"wasm freestanding compile failed\nstderr:\n{cp.stderr}")

    return ok()


def kind_arena_grow_test(ctx: RunContext, work_dir: Path) -> tuple[bool, str]:
    src_header = ctx.build_dir / "libsl.h"
    if not src_header.exists():
        return fail(f"missing generated header: {src_header}")
    if not ARENA_GROW_TEST_C_PATH.exists():
        return fail(f"missing source file: {ARENA_GROW_TEST_C_PATH}")

    shutil.copy2(src_header, work_dir / "libsl.h")
    c_path = work_dir / "arena_grow_test.c"
    exe_path = work_dir / "arena_grow_test"
    shutil.copy2(ARENA_GROW_TEST_C_PATH, c_path)

    args = [
        ctx.cc,
        "-std=c11",
        "-Wall",
        "-Wextra",
        "-Werror",
        str(c_path),
        "-o",
        str(exe_path),
    ]
    cp = run_cmd(args, cwd=work_dir)
    if cp.returncode != 0:
        return fail(f"arena_grow_test compile failed\nstderr:\n{cp.stderr}")

    run_cp = run_cmd([str(exe_path)], cwd=work_dir)
    if run_cp.returncode != 0:
        return fail(
            f"arena_grow_test failed with exit {run_cp.returncode}\n"
            f"stdout:\n{run_cp.stdout}\nstderr:\n{run_cp.stderr}"
        )
    return ok()


def execute_case(ctx: RunContext, case: TestCase, temp_root: Path) -> RunResult:
    start = time.time()
    work_dir = temp_root / f"{case.index:04d}-{sanitize_name(case.id)}"
    work_dir.mkdir(parents=True, exist_ok=True)

    c = case.data
    k = case.kind

    try:
        if k == "slc_stdout_eq":
            ok_main, detail = kind_slc_stdout_eq(ctx, c)
        elif k == "slc_ok":
            ok_main, detail = kind_slc_ok(ctx, c)
        elif k == "slc_fail_stderr":
            ok_main, detail = kind_slc_fail_stderr(ctx, c)
        elif k == "slc_ok_stderr":
            ok_main, detail = kind_slc_ok_stderr(ctx, c)
        elif k == "slc_fail_no_stdout":
            ok_main, detail = kind_slc_fail_no_stdout(ctx, c)
        elif k == "compile_only":
            ok_main, detail = kind_compile_only(ctx, c, work_dir)
        elif k == "compile_and_run":
            ok_main, detail = kind_compile_and_run(ctx, c, work_dir)
        elif k == "slc_run":
            ok_main, detail = kind_slc_run(ctx, c)
        elif k == "genpkg_text_check":
            ok_main, detail = kind_genpkg_text_check(ctx, c)
        elif k == "genpkg_compile":
            ok_main, detail = kind_genpkg_compile(ctx, c, work_dir)
        elif k == "libsl_freestanding":
            ok_main, detail = kind_libsl_freestanding(ctx, work_dir)
        elif k == "arena_grow_test":
            ok_main, detail = kind_arena_grow_test(ctx, work_dir)
        else:
            ok_main, detail = fail(f"unknown kind: {k}")

        if ok_main:
            ok_sidecar, sidecar_detail = maybe_check_codegen_sidecar(ctx, c)
            if not ok_sidecar:
                ok_main, detail = False, sidecar_detail
            elif sidecar_detail:
                detail = sidecar_detail

    except Exception as e:  # noqa: BLE001
        ok_main, detail = False, f"exception: {e}"

    return RunResult(case=case, ok=ok_main, duration=time.time() - start, detail=detail)


def load_cases(manifest: Path) -> List[TestCase]:
    if not manifest.exists():
        raise SystemExit(f"manifest not found: {manifest}")

    cases: List[TestCase] = []
    ids: set[str] = set()
    with manifest.open() as f:
        for lineno, line in enumerate(f, start=1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                data = json.loads(line)
            except json.JSONDecodeError as e:
                raise SystemExit(f"invalid JSON at {manifest}:{lineno}: {e}") from e

            if not isinstance(data, dict):
                raise SystemExit(f"expected object at {manifest}:{lineno}")
            for k in ("id", "suite", "kind"):
                if k not in data:
                    raise SystemExit(f"missing field {k!r} at {manifest}:{lineno}")
            case_id = str(data["id"])
            if case_id in ids:
                raise SystemExit(f"duplicate test id {case_id!r} at {manifest}:{lineno}")
            ids.add(case_id)
            cases.append(TestCase(index=len(cases), data=data))

    return cases


def filter_cases(cases: List[TestCase], suites: List[str], id_filters: List[str]) -> List[TestCase]:
    out = cases
    if suites:
        out = [c for c in out if c.suite in suites]
    if id_filters:
        out = [c for c in out if any(s in c.id for s in id_filters)]
    return out


def cmd_list(args: argparse.Namespace) -> int:
    cases = load_cases(abs_path(args.manifest))
    cases = filter_cases(cases, args.suite, args.id_contains)
    for c in cases:
        print(f"{c.id}\t{c.suite}\t{c.kind}")
    print(f"\n{len(cases)} test(s)")
    return 0


def lint_case_fields(case: TestCase) -> List[str]:
    errors: List[str] = []
    c = case.data
    for field in ("input", "expect", "expect_stderr", "harness"):
        v = c.get(field)
        if isinstance(v, str) and v and field != "harness":
            p = abs_path(v)
            if field == "input":
                if not p.exists():
                    errors.append(f"{case.id}: missing input path {v}")
            else:
                if not p.is_file():
                    errors.append(f"{case.id}: missing file {v}")
        if field == "harness" and isinstance(v, str) and v:
            p = abs_path(v)
            if not p.is_file():
                errors.append(f"{case.id}: missing harness file {v}")

    return errors


def iter_manifest_test_paths(cases: List[TestCase]) -> List[str]:
    paths: List[str] = []
    for case in cases:
        c = case.data
        for field in ("input", "expect", "expect_stderr", "harness"):
            v = c.get(field)
            if not isinstance(v, str) or not v:
                continue
            path = Path(v).as_posix().rstrip("/")
            if path.startswith("tests/"):
                paths.append(path)
    return paths


def is_test_root_artifact_file(path: Path) -> bool:
    if path.name.endswith(".expected.c"):
        return True
    return path.suffix in TEST_ROOT_TRACKED_SUFFIXES


def is_test_root_artifact_dir(path: Path) -> bool:
    # Treat top-level directories containing SL sources as test artifacts.
    for p in path.rglob("*.sl"):
        if p.is_file():
            return True
    return False


def lint_manifest_coverage(cases: List[TestCase]) -> List[str]:
    covered_roots: set[str] = set()
    for path in iter_manifest_test_paths(cases):
        parts = Path(path).parts
        if len(parts) >= 2:
            covered_roots.add(parts[1])

    errors: List[str] = []
    tests_dir = ROOT / "tests"
    for child in sorted(tests_dir.iterdir(), key=lambda p: p.name):
        if child.name in TEST_ROOT_IGNORED_NAMES:
            continue

        if child.is_file():
            if not is_test_root_artifact_file(child):
                continue
        elif child.is_dir():
            if not is_test_root_artifact_dir(child):
                continue
        else:
            continue

        if child.name not in covered_roots:
            errors.append(f"unmanifested test artifact: tests/{child.name}")

    return errors


def cmd_lint(args: argparse.Namespace) -> int:
    cases = load_cases(abs_path(args.manifest))
    errors: List[str] = []
    for case in cases:
        errors.extend(lint_case_fields(case))
    errors.extend(lint_manifest_coverage(cases))

    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        print(f"\n{len(errors)} lint error(s)", file=sys.stderr)
        return 1

    print(f"manifest OK: {len(cases)} test(s)")
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    manifest = abs_path(args.manifest)
    cases = load_cases(manifest)
    cases = filter_cases(cases, args.suite, args.id_contains)
    if not cases:
        print("no tests selected")
        return 0

    build_dir = abs_path(args.build_dir)
    slc = build_dir / "slc"
    if not slc.exists():
        print(f"slc binary not found: {slc}", file=sys.stderr)
        return 1

    ctx = RunContext(
        build_dir=build_dir,
        slc=slc,
        cc=args.cc,
        update=args.update,
        sidecar_codegen=not args.no_sidecar_codegen,
    )

    jobs = args.jobs if args.jobs and args.jobs > 0 else (os.cpu_count() or 1)
    temp_root = Path(tempfile.mkdtemp(prefix="slang-tests."))

    print(f"Running {len(cases)} test(s) with {jobs} job(s)")
    start = time.time()
    results: List[RunResult] = []

    try:
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = {ex.submit(execute_case, ctx, c, temp_root): c for c in cases}
            for fut in as_completed(futs):
                results.append(fut.result())
    finally:
        if args.keep_temp:
            print(f"kept temp dir: {temp_root}")
        else:
            shutil.rmtree(temp_root, ignore_errors=True)

    results.sort(key=lambda r: r.case.index)
    failed = [r for r in results if not r.ok]

    if failed:
        for r in failed:
            print(f"[FAIL] {r.case.id} ({r.case.suite}, {r.case.kind})", file=sys.stderr)
            if r.detail:
                print(r.detail, file=sys.stderr)
                print("", file=sys.stderr)
        print(
            f"{len(results) - len(failed)} passed, {len(failed)} failed in {time.time() - start:.2f}s",
            file=sys.stderr,
        )
        return 1

    print(f"tests passed ({len(results)} total) in {time.time() - start:.2f}s")
    return 0


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="SL test runner")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("--manifest", default=str(DEFAULT_MANIFEST.relative_to(ROOT)))
        sp.add_argument("--suite", action="append", default=[], help="exact suite name")
        sp.add_argument("--id-contains", action="append", default=[], help="filter by id substring")

    sp = sub.add_parser("list", help="list tests")
    add_common(sp)
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("lint", help="lint manifest")
    add_common(sp)
    sp.set_defaults(func=cmd_lint)

    sp = sub.add_parser("run", help="run tests")
    add_common(sp)
    sp.add_argument("--build-dir", default="_build/macos-aarch64-debug")
    sp.add_argument("--cc", default="clang")
    sp.add_argument("--jobs", type=int, default=0)
    sp.add_argument("--update", action="store_true", help="update *.expected.c sidecars")
    sp.add_argument("--no-sidecar-codegen", action="store_true")
    sp.add_argument("--keep-temp", action="store_true")
    sp.set_defaults(func=cmd_run)

    return p


def main(argv: List[str]) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
