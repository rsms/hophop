# Test Manifest

This repository uses a JSON Lines manifest at `tests/tests.jsonl`.
Each non-empty line is one JSON object describing a single test case.

The test runner is `tools/test.py`.

## Basic shape

Required fields for every entry:

- `id` (string): unique test identifier
- `suite` (string): logical category for filtering and reporting
- `kind` (string): runner action/type

Common optional fields:

- `input` (string): `.hop` file or package path
- `mode` (string): `hop` mode (`_`, `ast`, `check`, `build`)
- `no_import` (bool, default `false`): for `mode:"check"`, run `hop check --no-import`
- `platform` (string): pass `--platform` for package commands
- `arch` (string): pass `--arch` for package commands
- `testing` (bool): pass the internal `--testing` flag for package commands
- `exec_limit` (integer): per-execution wall-clock limit in milliseconds; if exceeded, the runner kills the current test process and fails the execution
- `expect` (string): expected output file (for stdout golden tests)
- `expect_stderr` (string): expected stderr golden file
- expected output files may use `{{ROOT}}` for the repository root path
- `contains` (array of strings): required substrings in generated text
- `not_contains` (array of strings): forbidden substrings in generated text
- `regex_count` (array of objects): `{"pattern": "...", "count": N}` checks on generated text
- `harness` (string): C harness template (`.c.in`) used for codegen compile smoke tests

## Supported `kind` values

- `hop_stdout_eq`
- `hop_ok`
- `hop_fail_stderr`
- `hop_ok_stderr`
- `hop_fail_no_stdout`
- `hop_cli`
- `hop_fmt`
- `compile_only`
- `compile_cache_reuse`
- `compile_and_run`
- `hop_run`
- `genpkg_text_check` (legacy test kind; runs `hop build --output-format c`)
- `genpkg_compile` (legacy test kind; runs `hop build --output-format c`)
- `genpkg_wasm_check` (legacy test kind; runs `hop build --platform ... -o out.wasm`)
- `libhop_freestanding`
- `builtin_h_freestanding`
- `arena_grow_test`

Kind-specific fields:

### `hop_fmt`

Required:

- `input` (string): file or directory fixture copied into temp workdir before running `hop fmt`

Optional:

- `check` (bool, default `false`): run `hop fmt --check`
- `expect_exit` (int, default `0`)
- `expect_stdout` (string): golden stdout file to compare
- `stderr_empty` (bool, default `true`)
- `verify_files` (array): each item is `{"path":"<relative path in temp workdir>","expect":"<golden file>"}` and is compared after command execution
- `pretest_fmt_exempt_input` (bool, default `false`): exclude this fixture input from the global pre-test formatter gate
- `pretest_fmt_exempt_paths` (array of strings): for directory inputs, exclude specific relative `.hop` paths from the global pre-test formatter gate

## Pre-test formatting gate

`tools/test.py run` executes a repository formatting gate before running tests:

- runs `hop fmt --check` against recursive `*.hop` files under `examples/`, `lib/`, and `tests/`
- always excludes `tests/fmt_canonical.hop`
- applies `pretest_fmt_exempt_input` / `pretest_fmt_exempt_paths` from `hop_fmt` manifest entries
- reports each dirty file with an actionable command to fix formatting

Some negative fixtures are intentionally not formatter-compatible; those are skipped when `hop fmt --check` emits diagnostics for that file.

### `hop_cli`

Runs `hop` with an explicit raw argument list for CLI behavior tests.

Required:

- `args` (array of strings): argv entries after the executable name
- `args` entries may use `{{ROOT}}` to refer to the repository root

Optional:

- `argv0_basename` (string): create a temporary symlink with this basename and invoke `hop` through it
- `expect_exit` (int, default `0`)
- `expect_stdout` (string): exact expected stdout file
- `expect_stderr` (string): exact expected stderr file
- `stdout_empty` (bool, default `true` unless `expect_stdout` is set)
- `stderr_empty` (bool, default `true` unless `expect_stderr` is set)

### `compile_and_run`

Optional:

- `expect_exit` (int, default `0`)
- `expect_nonzero` (bool)
- `run_stdout_empty` (bool, default `true`)
- `run_stderr_empty` (bool, default `true`)
- `stderr_contains` (string)
- `eval_expect` (string): `pass` or `fail` for `hop run --platform cli-eval`
- `eval_expect_exit` (int): expected `cli-eval` exit code when `eval_expect` is `pass`
- `eval_expect_nonzero` (bool): require non-zero `cli-eval` exit when `eval_expect` is `pass`
- `eval_stderr_contains` (string): expected substring in `cli-eval` stderr

### `hop_run`

Optional:

- `platform` (string): `hop --platform` target override for the run
- `env` (object): environment variables to add or override for the command
- `expect_exit` (int, default `0`)
- `expect_nonzero` (bool)
- `stdout_empty` (bool, default `true`)
- `stderr_empty` (bool, default `true`)
- `expect_stdout` (string): exact expected stdout file
- `expect_stderr` (string): exact expected stderr file
- `stderr_contains` (string)
- `eval_expect` (string): `pass` or `fail` for `hop run --platform cli-eval`
- `eval_expect_exit` (int): expected `cli-eval` exit code when `eval_expect` is `pass`
- `eval_expect_nonzero` (bool): require non-zero `cli-eval` exit when `eval_expect` is `pass`
- `eval_stderr_contains` (string): expected substring in `cli-eval` stderr

Note:

- `platform: "wasm-min"` uses the bundled Node-based Wasm smoke-test runner.
- `platform: "playbit"` shells out to `pb run <temp>.wasm` and requires a local Playbit
  `pb` binary (or `HOP_PLAYBIT_PB`).

## Codegen golden sidecars

For any manifest entry with `input` ending in `.hop`, the runner automatically checks for
`<input>.expected.c` sidecar.

Example: `tests/foo.hop` + `tests/foo.expected.c`

When present, the runner executes `hop build --output-format c` and diffs actual output against
`foo.expected.c`.

Use `tools/test.py run --update` to rewrite `.expected.c` files from current output.

### `genpkg_wasm_check`

Runs `hop build --output-format executable --platform <wasm-platform> <input> -o <temp-output>`
and validates the emitted Wasm file structurally.

Optional:

- `platform` (string): `hop --platform` target override for codegen
- `expect_section_ids` (array of integers): exact section id sequence
- `expect_exports` (array of strings): exact export name list
- `expect_imports` (array of strings): exact imported `module.field` name list
- `expect_func_count` (integer): expected function section count
- `expect_playbit_log_stdout` (string): for `platform: "playbit"`, instantiate the module in
  Node with a minimal Playbit syscall harness and compare captured log writes to this file

## Typical usage

- List all tests:
  - `python3 tools/test.py list`
- Run all tests:
  - `python3 tools/test.py run --build-dir _build/macos-aarch64-debug --cc clang`
- Run evaluator-only-compatible tests against an evaluator-only build:
  - `python3 tools/test.py run --build-dir _build/macos-aarch64-debug --cc clang --eval-only`
- Run one suite:
  - `python3 tools/test.py run --suite spec.lex_parse_check --build-dir _build/macos-aarch64-debug`
- Lint manifest paths/shape and unmanifested top-level test artifacts:
  - `python3 tools/test.py lint`

## Evaluator-only builds

If `hop` was built without the C backend, run the test runner with `--eval-only`.
This skips C-backend-only executions while still running parse/typecheck/fmt coverage plus
all `cli-eval` coverage.
