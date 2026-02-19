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

- `input` (string): `.sl` file or package path
- `mode` (string): `slc` mode (`_`, `ast`, `check`, `checkpkg`, `genpkg`, `genpkg:c`)
- `expect` (string): expected output file (for stdout golden tests)
- `expect_stderr` (string): expected stderr golden file
- `contains` (array of strings): required substrings in generated text
- `not_contains` (array of strings): forbidden substrings in generated text
- `regex_count` (array of objects): `{"pattern": "...", "count": N}` checks on generated text
- `harness` (string): C harness template (`.c.in`) used for codegen compile smoke tests

## Supported `kind` values

- `slc_stdout_eq`
- `slc_ok`
- `slc_fail_stderr`
- `slc_ok_stderr`
- `slc_fail_no_stdout`
- `compile_only`
- `compile_and_run`
- `slc_run`
- `genpkg_text_check`
- `genpkg_compile`
- `libsl_freestanding`
- `arena_grow_test`

Kind-specific fields:

### `compile_and_run`

Optional:

- `expect_exit` (int, default `0`)
- `expect_nonzero` (bool)
- `run_stdout_empty` (bool, default `true`)
- `run_stderr_empty` (bool, default `true`)
- `stderr_contains` (string)

### `slc_run`

Optional:

- `expect_exit` (int, default `0`)
- `expect_nonzero` (bool)
- `stdout_empty` (bool, default `true`)
- `stderr_empty` (bool, default `true`)
- `stderr_contains` (string)

## Codegen golden sidecars

For any manifest entry with `input` ending in `.sl`, the runner automatically checks for
`<input>.expected.c` sidecar.

Example: `tests/foo.sl` + `tests/foo.expected.c`

When present, the runner executes `slc genpkg:c` and diffs actual output against
`foo.expected.c`.

Use `tools/test.py run --update` to rewrite `.expected.c` files from current output.

## Typical usage

- List all tests:
  - `python3 tools/test.py list`
- Run all tests:
  - `python3 tools/test.py run --build-dir _build/macos-aarch64-debug --cc clang`
- Run one suite:
  - `python3 tools/test.py run --suite spec.lex_parse_check --build-dir _build/macos-aarch64-debug`
- Lint manifest paths/shape and unmanifested top-level test artifacts:
  - `python3 tools/test.py lint`
