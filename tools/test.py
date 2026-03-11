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
from typing import Any, Dict, List, Optional, Set, Tuple

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tests" / "tests.jsonl"
EXAMPLES_ROOT = ROOT / "examples"
ARENA_GROW_TEST_C_PATH = ROOT / "tests" / "harness" / "arena_grow_test.c"
BUILTIN_H_PATH = ROOT / "lib" / "builtin" / "builtin.h"
TEST_ROOT_IGNORED_NAMES = {"tests.jsonl", "README.md", "harness", ".DS_Store"}
TEST_ROOT_TRACKED_SUFFIXES = {".sl", ".stderr", ".ast", ".tokens"}
PRETEST_FMT_ROOTS = ("examples", "lib", "tests")
PRETEST_FMT_STRICT_ROOTS = ("examples", "lib")
PRETEST_FMT_DEFAULT_EXCLUDES = {"tests/fmt_canonical.sl"}
DIAGNOSTICS_JSON = ROOT / "src" / "diagnostics.jsonl"
DIAG_ID_RE = re.compile(r"\bSL\d{4}\b")
MAIN_FN_RE = re.compile(r"(?m)^\s*fn\s+main\s*\(")
EXAMPLE_COMPILE_SKIP: Set[str] = {
    # Known C backend gaps (tracked outside test discovery wiring).
    "examples/anytype.sl",
    "examples/string-format.sl",
    "examples/string-literal.sl",
    "examples/tuples.sl",
}


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
    case: "ExecutionCase"
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


@dataclass
class ExecutionCase:
    index: int
    fixture: TestCase
    variant: str
    id: str

    @property
    def suite(self) -> str:
        return self.fixture.suite

    @property
    def kind(self) -> str:
        if self.variant == "default":
            return self.fixture.kind
        return f"{self.fixture.kind}[{self.variant}]"


def fail(msg: str) -> tuple[bool, str]:
    return False, msg


def ok(msg: str = "") -> tuple[bool, str]:
    return True, msg


def count_noun(count: int, singular: str, plural: Optional[str] = None) -> str:
    if count == 1:
        return f"{count} {singular}"
    return f"{count} {plural or (singular + 's')}"


def abs_path(path: str) -> Path:
    p = Path(path)
    if p.is_absolute():
        return p
    return ROOT / p


def read_text(path: str) -> str:
    return abs_path(path).read_text()


def load_warning_diag_ids(path: Path) -> Set[str]:
    ids: Set[str] = set()
    for line in path.read_text().splitlines():
        s = line.strip()
        if not s or s.startswith("//"):
            continue
        obj = json.loads(s)
        if obj.get("type") == "warning":
            diag_id = obj.get("id")
            if isinstance(diag_id, str):
                ids.add(diag_id)
    return ids


WARNING_DIAG_IDS = load_warning_diag_ids(DIAGNOSTICS_JSON)


def strip_warning_diagnostics(stderr_text: str) -> str:
    if not stderr_text:
        return stderr_text
    out: List[str] = []
    skip_tip = False
    for line in stderr_text.splitlines(keepends=True):
        m = DIAG_ID_RE.search(line)
        if m is not None and m.group(0) in WARNING_DIAG_IDS:
            skip_tip = True
            continue
        if skip_tip and line.startswith("  tip:"):
            continue
        skip_tip = False
        out.append(line)
    return "".join(out)


def run_cmd(args: List[str], cwd: Optional[Path] = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=str(cwd or ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )


def normalize_rel_path(path: str) -> str:
    return Path(path).as_posix().rstrip("/")


def case_signature(data: Dict[str, Any]) -> tuple[str, str, str]:
    kind = str(data.get("kind", ""))
    mode = str(data.get("mode", ""))
    input_path = str(data.get("input", ""))
    return kind, mode, normalize_rel_path(input_path)


def has_main_function(path: Path) -> bool:
    if path.is_file():
        return MAIN_FN_RE.search(path.read_text()) is not None
    if path.is_dir():
        for source_path in sorted(path.rglob("*.sl")):
            if source_path.is_file() and MAIN_FN_RE.search(source_path.read_text()) is not None:
                return True
    return False


def discover_example_targets() -> tuple[List[str], List[str]]:
    files: List[str] = []
    package_dirs: Set[str] = set()

    if not EXAMPLES_ROOT.exists():
        return files, []

    for path in sorted(EXAMPLES_ROOT.glob("*.sl")):
        if path.is_file():
            files.append(path.relative_to(ROOT).as_posix())

    for path in sorted(EXAMPLES_ROOT.rglob("*.sl")):
        if not path.is_file():
            continue
        parent = path.parent
        if parent == EXAMPLES_ROOT:
            continue
        package_dirs.add(parent.relative_to(ROOT).as_posix())

    return files, sorted(package_dirs)


def build_auto_example_cases(manifest_cases: List[TestCase]) -> List[TestCase]:
    files, package_dirs = discover_example_targets()
    existing_ids: Set[str] = {case.id for case in manifest_cases}
    existing_signatures: Set[tuple[str, str, str]] = {
        case_signature(case.data) for case in manifest_cases
    }
    generated: List[TestCase] = []
    next_index = len(manifest_cases)

    def append_case(data: Dict[str, Any]) -> None:
        nonlocal next_index
        sig = case_signature(data)
        case_id = str(data["id"])
        if case_id in existing_ids or sig in existing_signatures:
            return
        existing_ids.add(case_id)
        existing_signatures.add(sig)
        generated.append(TestCase(index=next_index, data=data))
        next_index += 1

    for rel in files:
        slug = sanitize_name(rel)
        append_case(
            {
                "id": f"auto.examples.checkpkg.file.{slug}",
                "suite": "integration.examples.auto.checkpkg",
                "kind": "slc_ok",
                "mode": "checkpkg",
                "input": rel,
            }
        )
        if rel not in EXAMPLE_COMPILE_SKIP:
            append_case(
                {
                    "id": f"auto.examples.compile.file.{slug}",
                    "suite": "integration.examples.auto.compile",
                    "kind": "compile_only",
                    "input": rel,
                }
            )

    for rel in package_dirs:
        abs_pkg = abs_path(rel)
        slug = sanitize_name(rel)
        append_case(
            {
                "id": f"auto.examples.checkpkg.pkg.{slug}",
                "suite": "integration.examples.auto.checkpkg",
                "kind": "slc_ok",
                "mode": "checkpkg",
                "input": rel,
            }
        )
        if has_main_function(abs_pkg):
            append_case(
                {
                    "id": f"auto.examples.compile.pkg.{slug}",
                    "suite": "integration.examples.auto.compile",
                    "kind": "compile_only",
                    "input": rel,
                }
            )

    return generated


def collect_pretest_fmt_exemptions(cases: List[TestCase]) -> Tuple[Set[str], List[str]]:
    exempt_paths: Set[str] = set(PRETEST_FMT_DEFAULT_EXCLUDES)
    errors: List[str] = []

    for case in cases:
        if case.kind != "slc_fmt":
            continue
        c = case.data
        input_value = c.get("input")
        if not isinstance(input_value, str) or not input_value:
            continue

        input_rel = normalize_rel_path(input_value)
        input_abs = abs_path(input_rel)

        exempt_input = c.get("pretest_fmt_exempt_input", False)
        if not isinstance(exempt_input, bool):
            errors.append(f"{case.id}: pretest_fmt_exempt_input must be a bool")
            exempt_input = False

        if exempt_input:
            if input_abs.is_file():
                exempt_paths.add(input_rel)
            elif input_abs.is_dir():
                for p in input_abs.rglob("*.sl"):
                    if p.is_file():
                        exempt_paths.add(p.relative_to(ROOT).as_posix())
            else:
                errors.append(
                    f"{case.id}: pretest_fmt_exempt_input requires existing input path"
                )

        raw_exempt_paths = c.get("pretest_fmt_exempt_paths")
        if raw_exempt_paths is None:
            continue
        if not isinstance(raw_exempt_paths, list):
            errors.append(f"{case.id}: pretest_fmt_exempt_paths must be an array of strings")
            continue
        if not input_abs.is_dir():
            errors.append(
                f"{case.id}: pretest_fmt_exempt_paths requires directory input, got {input_rel}"
            )
            continue

        input_root = input_abs.resolve()
        for item in raw_exempt_paths:
            if not isinstance(item, str) or not item:
                errors.append(f"{case.id}: pretest_fmt_exempt_paths entries must be non-empty strings")
                continue
            if Path(item).is_absolute():
                errors.append(
                    f"{case.id}: pretest_fmt_exempt_paths entry must be relative: {item}"
                )
                continue

            normalized_item = os.path.normpath(item)
            if normalized_item in ("", "."):
                errors.append(f"{case.id}: invalid pretest_fmt_exempt_paths entry: {item!r}")
                continue
            if normalized_item == ".." or normalized_item.startswith(f"..{os.sep}"):
                errors.append(
                    f"{case.id}: pretest_fmt_exempt_paths entry escapes input directory: {item}"
                )
                continue

            candidate = (input_abs / normalized_item).resolve()
            try:
                candidate.relative_to(input_root)
            except ValueError:
                errors.append(
                    f"{case.id}: pretest_fmt_exempt_paths entry escapes input directory: {item}"
                )
                continue
            if candidate.suffix != ".sl":
                errors.append(f"{case.id}: pretest_fmt_exempt_paths entry is not an .sl file: {item}")
                continue
            if not candidate.is_file():
                errors.append(
                    f"{case.id}: pretest_fmt_exempt_paths entry does not exist: {item}"
                )
                continue
            exempt_paths.add(candidate.relative_to(ROOT).as_posix())

    return exempt_paths, errors


def collect_pretest_fmt_targets(exempt_paths: Set[str]) -> List[str]:
    targets: List[str] = []
    for root_name in PRETEST_FMT_ROOTS:
        root_path = ROOT / root_name
        if not root_path.exists():
            continue
        for path in root_path.rglob("*.sl"):
            if not path.is_file():
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel in exempt_paths:
                continue
            targets.append(rel)
    targets.sort()
    return targets


def is_pretest_fmt_strict_target(path: str) -> bool:
    normalized = normalize_rel_path(path)
    for root_name in PRETEST_FMT_STRICT_ROOTS:
        if normalized == root_name or normalized.startswith(root_name + "/"):
            return True
    return False


def run_pretest_fmt_target(ctx: RunContext, path: str) -> tuple[str, subprocess.CompletedProcess[str]]:
    return path, run_cmd([str(ctx.slc), "fmt", "--check", path])


def run_pretest_fmt_check(ctx: RunContext, all_cases: List[TestCase], jobs: int) -> tuple[bool, str]:
    exempt_paths, config_errors = collect_pretest_fmt_exemptions(all_cases)
    if config_errors:
        return fail("pre-test formatter config errors:\n" + "\n".join(config_errors))

    targets = collect_pretest_fmt_targets(exempt_paths)
    if not targets:
        return ok()

    dirty_paths: Set[str] = set()
    command_errors: List[str] = []

    results: Dict[str, subprocess.CompletedProcess[str]] = {}
    worker_count = min(max(jobs, 1), len(targets))
    if worker_count == 1:
        for path in targets:
            results[path] = run_cmd([str(ctx.slc), "fmt", "--check", path])
    else:
        with ThreadPoolExecutor(max_workers=worker_count) as ex:
            futs = {ex.submit(run_pretest_fmt_target, ctx, path): path for path in targets}
            for fut in as_completed(futs):
                path, cp = fut.result()
                results[path] = cp

    for path in targets:
        cp = results[path]
        stdout_lines = [line.strip() for line in cp.stdout.splitlines() if line.strip()]
        is_strict_target = is_pretest_fmt_strict_target(path)

        if cp.returncode == 0:
            continue
        if cp.stderr:
            if is_strict_target:
                stderr_first = cp.stderr.strip().splitlines()
                if stderr_first:
                    command_errors.append(f"{path}: fmt --check failed: {stderr_first[0]}")
                else:
                    command_errors.append(f"{path}: fmt --check failed")
            else:
                # Some tests fixtures are intentionally not formatter-compatible.
                continue
            continue
        if stdout_lines:
            normalized = normalize_rel_path(path)
            has_mismatch_report = any(
                line == path
                or line == normalized
                or line.startswith(path + ":")
                or line.startswith(normalized + ":")
                for line in stdout_lines
            )
            if has_mismatch_report:
                dirty_paths.add(path)
                continue
            path_like = [line for line in stdout_lines if line.endswith(".sl")]
            if path_like:
                dirty_paths.update(path_like)
                continue
            command_errors.append(
                f"{path}: unexpected fmt --check output without recognizable path marker"
            )
            continue
        command_errors.append(f"{path}: unexpected fmt --check failure without diagnostic output")

    if command_errors:
        return fail("pre-test formatting check errors:\n" + "\n".join(command_errors))

    if dirty_paths:
        details = [
            f"{path}: non-standard formatting; run `{ctx.slc} fmt {path}` to correct formatting"
            for path in sorted(dirty_paths)
        ]
        return fail("pre-test formatting check failed:\n" + "\n".join(details))

    return ok()


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
    stderr = strip_warning_diagnostics(cp.stderr)
    if cp.returncode != 0:
        return fail(f"unexpected failure (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if stderr:
        return fail(f"unexpected stderr:\n{stderr}")
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
    stderr = strip_warning_diagnostics(cp.stderr)
    if cp.returncode != 0:
        return fail(f"unexpected failure (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    if stderr:
        return fail(f"unexpected stderr:\n{stderr}")
    return ok()


def kind_slc_ok_tmp(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    source_path = abs_path(str(case["input"]))
    temp_dir = Path(tempfile.mkdtemp(prefix="slc-ok-tmp-"))
    temp_input = temp_dir / source_path.name
    try:
        shutil.copyfile(source_path, temp_input)
        cp = run_cmd(slc_args(ctx, str(case["mode"]), str(temp_input)))
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)
    stderr = strip_warning_diagnostics(cp.stderr)
    if cp.returncode != 0:
        return fail(f"unexpected failure (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    if stderr:
        return fail(f"unexpected stderr:\n{stderr}")
    return ok()


def kind_slc_fail_stderr(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    cp = run_cmd(slc_args(ctx, str(case["mode"]), str(case["input"])))
    stderr = strip_warning_diagnostics(cp.stderr)
    if cp.returncode == 0:
        return fail("expected failure but command succeeded")
    if cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    expected = read_text(str(case["expect_stderr"]))
    if stderr != expected:
        diff = "".join(
            difflib.unified_diff(
                expected.splitlines(True),
                stderr.splitlines(True),
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


def kind_slc_fmt(ctx: RunContext, case: Dict[str, Any], work_dir: Path) -> tuple[bool, str]:
    input_path = abs_path(str(case["input"]))
    check = bool(case.get("check", False))
    expect_exit = int(case.get("expect_exit", 0))
    stderr_empty = bool(case.get("stderr_empty", True))
    expected_stdout_path = case.get("expect_stdout")
    verify_files = case.get("verify_files", [])
    cmd_target: str

    if input_path.is_dir():
        dst_dir = work_dir / "input"
        shutil.copytree(input_path, dst_dir)
        cmd_target = "input"
    else:
        dst_file = work_dir / "input.sl"
        shutil.copy2(input_path, dst_file)
        cmd_target = "input.sl"

    args = [str(ctx.slc), "fmt"]
    if check:
        args.append("--check")
    args.append(cmd_target)
    cp = run_cmd(args, cwd=work_dir)

    if cp.returncode != expect_exit:
        return fail(f"unexpected exit code: expected {expect_exit}, got {cp.returncode}")
    if stderr_empty and cp.stderr:
        return fail(f"unexpected stderr:\n{cp.stderr}")

    if expected_stdout_path is not None:
        expected_stdout = read_text(str(expected_stdout_path))
        if cp.stdout != expected_stdout:
            diff = "".join(
                difflib.unified_diff(
                    expected_stdout.splitlines(True),
                    cp.stdout.splitlines(True),
                    fromfile=str(expected_stdout_path),
                    tofile="actual.stdout",
                )
            )
            return fail(f"stdout mismatch:\n{diff}")

    for item in verify_files:
        rel_path = str(item["path"])
        expect_path = str(item["expect"])
        actual_path = work_dir / rel_path
        if not actual_path.exists():
            return fail(f"missing expected output file: {rel_path}")
        actual = actual_path.read_text()
        expected = read_text(expect_path)
        if actual != expected:
            diff = "".join(
                difflib.unified_diff(
                    expected.splitlines(True),
                    actual.splitlines(True),
                    fromfile=expect_path,
                    tofile=rel_path,
                )
            )
            return fail(f"formatted content mismatch for {rel_path}:\n{diff}")

    return ok()


def run_compile(ctx: RunContext, input_path: str, output_path: Path) -> subprocess.CompletedProcess[str]:
    return run_cmd([str(ctx.slc), "compile", input_path, "-o", str(output_path)])


def run_compile_with_cache(
    ctx: RunContext, input_path: str, output_path: Path, cache_dir: Path
) -> subprocess.CompletedProcess[str]:
    return run_cmd(
        [
            str(ctx.slc),
            "--cache-dir",
            str(cache_dir),
            "compile",
            input_path,
            "-o",
            str(output_path),
        ]
    )


def kind_compile_only(ctx: RunContext, case: Dict[str, Any], work_dir: Path) -> tuple[bool, str]:
    exe = work_dir / "program"
    cp = run_compile(ctx, str(case["input"]), exe)
    stderr = strip_warning_diagnostics(cp.stderr)
    if cp.returncode != 0:
        return fail(f"compile failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if case.get("compile_stdout_empty", False) and cp.stdout:
        return fail(f"unexpected compile stdout:\n{cp.stdout}")
    if case.get("compile_stderr_empty", False) and stderr:
        return fail(f"unexpected compile stderr:\n{stderr}")
    if not exe.exists() or not os.access(exe, os.X_OK):
        return fail("compile output executable missing")
    return ok()


def kind_compile_cache_reuse(
    ctx: RunContext, case: Dict[str, Any], work_dir: Path
) -> tuple[bool, str]:
    exe = work_dir / "program"
    cache_dir = work_dir / "cache"
    cache_pkg_dir = cache_dir / "v1" / "pkg"

    cp1 = run_compile_with_cache(ctx, str(case["input"]), exe, cache_dir)
    stderr1 = strip_warning_diagnostics(cp1.stderr)
    if cp1.returncode != 0:
        return fail(f"compile failed (first run, exit {cp1.returncode})\nstderr:\n{cp1.stderr}")
    if case.get("compile_stdout_empty", False) and cp1.stdout:
        return fail(f"unexpected compile stdout:\n{cp1.stdout}")
    if case.get("compile_stderr_empty", False) and stderr1:
        return fail(f"unexpected compile stderr:\n{stderr1}")
    if not exe.exists() or not os.access(exe, os.X_OK):
        return fail("compile output executable missing on first run")

    objs1 = sorted(cache_pkg_dir.rglob("pkg.o")) if cache_pkg_dir.exists() else []
    if not objs1:
        return fail("cache compile produced no package objects")
    mtimes1 = {str(p): p.stat().st_mtime_ns for p in objs1}

    time.sleep(1.1)

    cp2 = run_compile_with_cache(ctx, str(case["input"]), exe, cache_dir)
    stderr2 = strip_warning_diagnostics(cp2.stderr)
    if cp2.returncode != 0:
        return fail(f"compile failed (second run, exit {cp2.returncode})\nstderr:\n{cp2.stderr}")
    if case.get("compile_stdout_empty", False) and cp2.stdout:
        return fail(f"unexpected compile stdout:\n{cp2.stdout}")
    if case.get("compile_stderr_empty", False) and stderr2:
        return fail(f"unexpected compile stderr:\n{stderr2}")
    if not exe.exists() or not os.access(exe, os.X_OK):
        return fail("compile output executable missing on second run")

    objs2 = sorted(cache_pkg_dir.rglob("pkg.o")) if cache_pkg_dir.exists() else []
    mtimes2 = {str(p): p.stat().st_mtime_ns for p in objs2}
    if set(mtimes1.keys()) != set(mtimes2.keys()):
        return fail("cache object set changed unexpectedly between runs")
    for path, mtime1 in mtimes1.items():
        mtime2 = mtimes2[path]
        if mtime2 != mtime1:
            return fail(f"cache object was rebuilt unexpectedly: {path}")

    return ok()


def kind_compile_and_run(ctx: RunContext, case: Dict[str, Any], work_dir: Path) -> tuple[bool, str]:
    exe = work_dir / "program"
    cp = run_compile(ctx, str(case["input"]), exe)
    stderr = strip_warning_diagnostics(cp.stderr)
    if cp.returncode != 0:
        return fail(f"compile failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if case.get("compile_stdout_empty", False) and cp.stdout:
        return fail(f"unexpected compile stdout:\n{cp.stdout}")
    if case.get("compile_stderr_empty", False) and stderr:
        return fail(f"unexpected compile stderr:\n{stderr}")
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


def run_slc_run_cmd(
    ctx: RunContext, input_path: str, platform: Optional[str]
) -> subprocess.CompletedProcess[str]:
    args = [str(ctx.slc)]
    if platform:
        args.extend(["--platform", platform])
    args.extend(["run", input_path])
    return run_cmd(args)


def kind_eval_run_expectation(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    eval_expect = case.get("eval_expect")
    if eval_expect not in ("pass", "fail"):
        return fail(f"invalid eval_expect value {eval_expect!r}; expected 'pass' or 'fail'")

    input_path = str(case["input"])
    cp = run_slc_run_cmd(ctx, input_path, "cli-eval")
    default_expect_nonzero = bool(case.get("expect_nonzero", False))
    default_expect_exit = int(case.get("expect_exit", 0))
    if eval_expect == "fail":
        if cp.returncode == 0:
            return fail("expected cli-eval run failure, but run succeeded")
    else:
        expect_nonzero = bool(case.get("eval_expect_nonzero", default_expect_nonzero))
        expect_exit = int(case.get("eval_expect_exit", default_expect_exit))
        if expect_nonzero:
            if cp.returncode == 0:
                return fail("expected non-zero cli-eval exit code")
        elif cp.returncode != expect_exit:
            return fail(
                f"unexpected cli-eval exit code: expected {expect_exit}, got {cp.returncode}"
            )

    eval_stderr_contains = case.get("eval_stderr_contains")
    if eval_stderr_contains is not None and str(eval_stderr_contains) not in cp.stderr:
        return fail(
            f"cli-eval stderr missing expected text {eval_stderr_contains!r}\n{cp.stderr}"
        )
    return ok()


def kind_slc_run(ctx: RunContext, case: Dict[str, Any]) -> tuple[bool, str]:
    platform = str(case["platform"]) if case.get("platform") is not None else None
    cp = run_slc_run_cmd(ctx, str(case["input"]), platform)
    stderr = strip_warning_diagnostics(cp.stderr)
    expect_nonzero = bool(case.get("expect_nonzero", False))
    expect_exit = int(case.get("expect_exit", 0))
    if expect_nonzero:
        if cp.returncode == 0:
            return fail("expected non-zero exit code")
    elif cp.returncode != expect_exit:
        return fail(f"unexpected exit code: expected {expect_exit}, got {cp.returncode}")

    if case.get("stdout_empty", True) and cp.stdout:
        return fail(f"unexpected stdout:\n{cp.stdout}")
    if case.get("stderr_empty", True) and stderr:
        return fail(f"unexpected stderr:\n{stderr}")
    stderr_contains = case.get("stderr_contains")
    if stderr_contains is not None and str(stderr_contains) not in stderr:
        return fail(f"stderr missing expected text {stderr_contains!r}\n{stderr}")

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
    stderr = strip_warning_diagnostics(cp.stderr)
    if cp.returncode != 0:
        return fail(f"{mode} failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if case.get("stderr_empty", True) and stderr:
        return fail(f"unexpected stderr:\n{stderr}")
    return check_text_constraints(cp.stdout, case)


def kind_genpkg_compile(ctx: RunContext, case: Dict[str, Any], work_dir: Path) -> tuple[bool, str]:
    mode = str(case.get("mode", "genpkg:c"))
    cp = run_genpkg_mode(ctx, mode, str(case["input"]))
    stderr = strip_warning_diagnostics(cp.stderr)
    if cp.returncode != 0:
        return fail(f"{mode} failed (exit {cp.returncode})\nstderr:\n{cp.stderr}")
    if case.get("stderr_empty", True) and stderr:
        return fail(f"unexpected stderr:\n{stderr}")

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


def kind_builtin_h_freestanding(ctx: RunContext, work_dir: Path) -> tuple[bool, str]:
    if not BUILTIN_H_PATH.exists():
        return fail(f"missing header: {BUILTIN_H_PATH}")

    builtin_h_c = work_dir / "builtin_h.c"
    builtin_h_c.write_text('#include "lib/builtin/builtin.h"\n')

    host_args = [
        ctx.cc,
        "-std=c11",
        "-ffreestanding",
        "-nostdlib",
        "-fno-builtin",
        "-I",
        str(ROOT),
        "-Wall",
        "-Wextra",
        "-Wno-comment",
        "-Werror",
        "-c",
        str(builtin_h_c),
        "-o",
        str(work_dir / "builtin_h.freestanding.o"),
    ]
    cp = run_cmd(host_args, cwd=work_dir)
    if cp.returncode != 0:
        return fail(
            "builtin.h freestanding compile failed; did you accidentally include libc headers? "
            "builtin is not permitted to use libc\n"
            f"stderr:\n{cp.stderr}"
        )

    wasm_args = [
        ctx.cc,
        "-target",
        "wasm32-unknown-unknown",
        "-std=c11",
        "-ffreestanding",
        "-nostdlib",
        "-fno-builtin",
        "-I",
        str(ROOT),
        "-Wall",
        "-Wextra",
        "-Wno-comment",
        "-Werror",
        "-c",
        str(builtin_h_c),
        "-o",
        str(work_dir / "builtin_h.wasm.o"),
    ]
    cp = run_cmd(wasm_args, cwd=work_dir)
    if cp.returncode != 0:
        return fail(
            "builtin.h wasm freestanding compile failed; did you accidentally include libc headers? "
            "builtin is not permitted to use libc\n"
            f"stderr:\n{cp.stderr}"
        )

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


def case_has_eval_variant(case: TestCase) -> bool:
    if case.kind not in ("compile_and_run", "slc_run"):
        return False
    if case.data.get("eval_expect") is None:
        return False
    return case.data.get("platform") != "cli-eval"


def expand_execution_cases(fixtures: List[TestCase]) -> List[ExecutionCase]:
    cases: List[ExecutionCase] = []
    for fixture in fixtures:
        has_eval = case_has_eval_variant(fixture)
        default_id = fixture.id if not has_eval else f"{fixture.id}[default]"
        cases.append(
            ExecutionCase(
                index=len(cases),
                fixture=fixture,
                variant="default",
                id=default_id,
            )
        )
        if has_eval:
            cases.append(
                ExecutionCase(
                    index=len(cases),
                    fixture=fixture,
                    variant="cli-eval",
                    id=f"{fixture.id}[cli-eval]",
                )
            )
    return cases


def mode_uses_c_backend(mode: Any) -> bool:
    if not isinstance(mode, str):
        return False
    return mode == "genpkg" or mode.startswith("genpkg:")


def execution_case_supported_in_eval_only(case: ExecutionCase) -> bool:
    fixture = case.fixture
    kind = fixture.kind
    data = fixture.data

    if case.variant == "cli-eval":
        return True

    if kind in (
        "compile_only",
        "compile_cache_reuse",
        "compile_and_run",
        "genpkg_text_check",
        "genpkg_compile",
    ):
        return False

    if kind == "slc_run":
        return data.get("platform") == "cli-eval"

    return not mode_uses_c_backend(data.get("mode"))


def execute_case(ctx: RunContext, case: ExecutionCase, temp_root: Path) -> RunResult:
    start = time.time()
    work_dir = temp_root / f"{case.index:04d}-{sanitize_name(case.id)}"
    work_dir.mkdir(parents=True, exist_ok=True)

    c = case.fixture.data
    k = case.fixture.kind

    try:
        if case.variant == "cli-eval":
            ok_main, detail = kind_eval_run_expectation(ctx, c)
        else:
            if k == "slc_stdout_eq":
                ok_main, detail = kind_slc_stdout_eq(ctx, c)
            elif k == "slc_ok":
                ok_main, detail = kind_slc_ok(ctx, c)
            elif k == "slc_ok_tmp":
                ok_main, detail = kind_slc_ok_tmp(ctx, c)
            elif k == "slc_fail_stderr":
                ok_main, detail = kind_slc_fail_stderr(ctx, c)
            elif k == "slc_ok_stderr":
                ok_main, detail = kind_slc_ok_stderr(ctx, c)
            elif k == "slc_fail_no_stdout":
                ok_main, detail = kind_slc_fail_no_stdout(ctx, c)
            elif k == "slc_fmt":
                ok_main, detail = kind_slc_fmt(ctx, c, work_dir)
            elif k == "compile_only":
                ok_main, detail = kind_compile_only(ctx, c, work_dir)
            elif k == "compile_cache_reuse":
                ok_main, detail = kind_compile_cache_reuse(ctx, c, work_dir)
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
            elif k == "builtin_h_freestanding":
                ok_main, detail = kind_builtin_h_freestanding(ctx, work_dir)
            elif k == "arena_grow_test":
                ok_main, detail = kind_arena_grow_test(ctx, work_dir)
            else:
                ok_main, detail = fail(f"unknown kind: {k}")

        if ok_main and case.variant == "default":
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

    cases.extend(build_auto_example_cases(cases))
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
    print(f"\n{count_noun(len(cases), 'test')}")
    return 0


def lint_case_fields(case: TestCase) -> List[str]:
    errors: List[str] = []
    c = case.data
    for field in ("input", "expect", "expect_stderr", "expect_stdout", "harness"):
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

    verify_files = c.get("verify_files")
    if isinstance(verify_files, list):
        for item in verify_files:
            if not isinstance(item, dict):
                errors.append(f"{case.id}: verify_files item must be an object")
                continue
            expect = item.get("expect")
            if isinstance(expect, str) and expect:
                p = abs_path(expect)
                if not p.is_file():
                    errors.append(f"{case.id}: missing verify_files expect file {expect}")

    return errors


def iter_manifest_test_paths(cases: List[TestCase]) -> List[str]:
    paths: List[str] = []
    for case in cases:
        c = case.data
        for field in ("input", "expect", "expect_stderr", "expect_stdout", "harness"):
            v = c.get(field)
            if not isinstance(v, str) or not v:
                continue
            path = Path(v).as_posix().rstrip("/")
            if path.startswith("tests/"):
                paths.append(path)
        verify_files = c.get("verify_files")
        if isinstance(verify_files, list):
            for item in verify_files:
                if not isinstance(item, dict):
                    continue
                expect = item.get("expect")
                if isinstance(expect, str) and expect:
                    path = Path(expect).as_posix().rstrip("/")
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
    _, fmt_errors = collect_pretest_fmt_exemptions(cases)
    errors.extend(fmt_errors)
    errors.extend(lint_manifest_coverage(cases))

    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        print(f"\n{count_noun(len(errors), 'lint error')}", file=sys.stderr)
        return 1

    print(f"manifest OK: {count_noun(len(cases), 'test')}")
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    manifest = abs_path(args.manifest)
    all_fixtures = load_cases(manifest)
    fixtures = filter_cases(all_fixtures, args.suite, args.id_contains)
    if not fixtures:
        print("no tests selected")
        return 0
    cases = expand_execution_cases(fixtures)
    if args.eval_only:
        cases = [case for case in cases if execution_case_supported_in_eval_only(case)]

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
        sidecar_codegen=not args.no_sidecar_codegen and not args.eval_only,
    )

    jobs = args.jobs if args.jobs and args.jobs > 0 else (os.cpu_count() or 1)

    fmt_ok, fmt_detail = run_pretest_fmt_check(ctx, all_fixtures, jobs)
    if not fmt_ok:
        print(fmt_detail, file=sys.stderr)
        return 1

    temp_root = Path(tempfile.mkdtemp(prefix="slang-tests."))

    print(
        f"Running {count_noun(len(fixtures), 'fixture')}, "
        f"{count_noun(len(cases), 'execution')} with {count_noun(jobs, 'job')}"
    )
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
            f"{count_noun(len(results) - len(failed), 'execution')} passed, "
            f"{count_noun(len(failed), 'execution')} failed in {time.time() - start:.2f}s",
            file=sys.stderr,
        )
        return 1

    print(
        f"all {count_noun(len(results), 'execution')} passed "
        f"({count_noun(len(fixtures), 'fixture')}) in {time.time() - start:.2f}s"
    )
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
    sp.add_argument("--eval-only", action="store_true", help="skip C-backend-only executions")
    sp.add_argument("--keep-temp", action="store_true")
    sp.set_defaults(func=cmd_run)

    return p


def main(argv: List[str]) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
