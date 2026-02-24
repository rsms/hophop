# Plan: Remove Special C Codegen for `print`

## Goal

Make `print` lower like a normal SL function call (using `lib/core/functions.sl`) and remove the hardcoded `print` branches from C codegen.

## Current state (as of commit `51b02e0`)

- Branch: `logger1`
- Full test suite passes at this commit (`./build.sh test` -> `270/270`).
- `print` still has 2 hardcoded lowering branches in `src/codegen_c.c`:
  - around line `5796` (direct call path)
  - around line `5921` (field-call fallback path)
- `print` source implementation exists in `lib/core/functions.sl`:
  - `context.log.handler(&context.log, message, LogLevel.Info, 0 as LogFlags)`

## Important recent context

- We removed dead `platform__console_log` codegen and updated docs in `51b02e0`.
- We tested removing `print` special-cases directly; this failed.
- Failure mode when those branches are removed:
  - generated C emitted `print(...)` directly
  - C compile failed with undeclared function `print`
  - test failures included:
    - `integration.runtime.print_ok.compile_and_run`
    - `integration.examples.anonymous_aggregates.compile_only`
    - `integration.examples.context.compile_only`
    - `integration.codegen.print_ok.lowering` (expected inline handler text no longer found)

This proves current generic call resolution/emission does not map builtin `print` to a concrete emitted C symbol/body yet.

## Likely root cause

- `print` is treated as a language builtin by frontend/typecheck, but generic C call emission path does not currently:
  - resolve it to a generated C function symbol (e.g. `core__print`)
  - and/or ensure that function body is emitted/available in generated output.

So codegen currently needs the hardcoded branch to avoid emitting raw `print(...)`.

## Implementation plan

1. Trace builtin call resolution for `print`
- Inspect `ResolveCallTarget`, `FindFnSigBySlice`, and related call collection in `src/codegen_c.c`.
- Confirm how other non-hardcoded functions become callable C symbols.
- Confirm whether `print` appears in function signature tables for packages importing `core`.

2. Ensure `print` resolves to a concrete callable symbol in normal path
- Update resolution logic so `print` call sites lower through `EmitResolvedCall` (or equivalent) with an actual C symbol.
- Ensure receiver-style fallback (`msg.print()` form if supported) resolves consistently.

3. Ensure implementation body is emitted
- Verify generated C/header contains a definition (or valid linkage) for resolved `print` symbol sourced from `lib/core/functions.sl`.
- If current architecture intentionally inlines builtins only, move `print` from “hardcoded builtin lowering” to “core function lowering.”

4. Remove both hardcoded `print` branches from `src/codegen_c.c`
- Delete direct-call branch.
- Delete field-call fallback branch.

5. Update tests to match desired lowering contract
- `integration.codegen.print_ok.lowering` currently expects inline handler call text from hardcoded lowering.
- Change expectation to reflect normal function-call lowering path.
- Keep runtime behavior checks for `print` unchanged.

6. Validate end-to-end
- Run `./build.sh test`.
- Spot-check generated output for `tests/print_ok.sl` using `slc genpkg:c` to confirm no hardcoded print expansion remains.

## Suggested execution checklist for a fresh agent

1. Start on a fresh worktree/branch.
2. Rebase/merge from `main` first (another agent recently committed `Context.temp_mem`, commit `1d945b2` on branch `context-temp_mem`; verify whether it has merged to main).
3. Implement steps above incrementally.
4. Run full test suite before final commit.

## Acceptance criteria

- No `print` special-case branches remain in `src/codegen_c.c`.
- `print` calls compile/run via normal call lowering.
- Full suite passes.
- `integration.codegen.print_ok.lowering` updated to validate the new lowering behavior.
