# HEP-19 implementation checklist (completed)

This checklist breaks HEP-19 into issue-sized tasks for parser, typechecker, codegen, tests, and
migration support.

Related proposal: `docs/HEP-19-pointer-slice-unification.md`.

Status: completed on February 21, 2026.

## Phase 0: gating and sequencing

- [x] Use a branch-based hard-switch rollout strategy (no compatibility feature flag).
- [x] Decide rollout mode:
  - hard switch (old forms removed immediately), or
  - compatibility window (old forms parsed with deprecation diagnostics).

Acceptance criteria:

- Build and existing test harness run in the selected rollout mode.

## Phase 1: parser and AST

- [x] Parse `[T]` as an unsized type form.
- [x] Preserve parsing of `[T N]` as fixed-size array value type.
- [x] Parse `*[T]` and `&[T]` as pointer/ref to unsized slice type.
- [x] Remove parse acceptance for `mut&T`, `mut&[T N]`, and `mut[T]` (or mark deprecated in
      compatibility mode).
- [x] Ensure pretty-print / AST dump output reflects new canonical type spelling.

Acceptance criteria:

- Parser unit tests cover all new forms and all removed/deprecated forms.
- `hop ast` output is stable and unambiguous for `*T` vs `*[T]`.

## Phase 2: type model and typechecker

- [x] Mark `[T]` as unsized and reject by-value unsized locals/params/returns.
- [x] Implement coercions:
  - `[T N] -> &[T]`
  - `[T N] -> *[T]` for mutable lvalues
  - `&[T N] -> &[T]`
  - `*[T N] -> *[T]`
  - `*[T] -> &[T]`
  - `*T -> &T`
- [x] Reject implicit `*T -> *[T]` and `&T -> &[T]`.
- [x] Re-bind mutability rules so writes require `*...` view forms.
- [x] Update selector/indexing typing for VSS dependent fields:
  - from `*Packet` field `data` yields `*[Elem]`
  - from `&Packet` field `data` yields `&[Elem]`
- [x] Update diagnostic text and fix-it hints from HEP-2 forms to HEP-19 forms.

Acceptance criteria:

- New typing tests pass for allowed coercions.
- Negative tests pass for illegal by-value unsized use and illegal implicit singleton-to-slice
  coercions.

## Phase 3: built-ins (`len`, `sizeof`)

- [x] Update `len` typing rules:
  - accept `[T N]`, `*[T]`, `&[T]`, `*[T N]`, `&[T N]`
  - reject `*T` and `&T`
- [x] Keep `sizeof(type T)` compile-time and reject unsized/variable-size by-value types.
- [x] Implement `sizeof(expr)` lowering:
  - fixed-size values: compile-time constant
  - `*T`/`&T` with fixed-size `T`: compile-time `sizeof(T)`
  - `*V`/`&V` with variable-size `V`: runtime helper
  - `*[T]`/`&[T]`: runtime `len * sizeof(T)`
  - `*[T N]`/`&[T N]`: compile-time `N * sizeof(T)`
- [x] Implement null handling policy for VSS `sizeof(expr)`:
  - safe mode traps
  - unsafe mode undefined behavior

Acceptance criteria:

- Built-in tests cover compile-time folding, runtime paths, and safe-mode null trap behavior.

## Phase 4: codegen and ABI

- [x] Confirm C representation for `*[T]`/`&[T]` as `(ptr,len)` remains ABI-stable.
- [x] Ensure lowering of slice operations (`index`, `slice`, `len`, `sizeof`) uses shared helpers.
- [x] Ensure VSS field accessor codegen returns slice pointer forms (`*[T]`/`&[T]`) consistently.
- [x] Audit generated C for wasm32/linux-musl/macos portability constraints.

Acceptance criteria:

- Generated C compiles cleanly for debug/release.
- No backend regressions in existing VSS tests.

## Phase 5: migration compatibility and docs

- [x] Apply source migration for old forms:
  - `mut&T` -> `*T`
  - `mut&[T N]` -> `*[T N]`
  - `mut[T]` -> `*[T]`
  - old `[T]` slice value -> `&[T]`
- [x] Decide deprecation timeline and remove compatibility parser paths when complete.
- [x] Update docs:
  - `docs/HEP-19-pointer-slice-unification.md`
  - `docs/HEP-19.1-implementation-checklist.md`
  - affected examples and tests

Acceptance criteria:

- Documentation and diagnostics agree on final syntax and semantics.

## Phase 6: tests and rollout

- [x] Add positive tests for:
  - slice pointer creation from arrays
  - coercion matrix
  - `len` and `sizeof(expr)` behavior by category
  - VSS field access with read-only and writable bases
- [x] Add negative tests for:
  - by-value `[T]`
  - illegal coercions
  - writing through `&...` views
- [x] Run full suite: `./build.sh test`.
- [x] Add targeted suites for parser/typechecker/codegen paths changed by HEP-19.

Acceptance criteria:

- Full suite passes.
- New tests protect all normative HEP-19 rules.

## Suggested issue breakdown

1. Parser + AST: unsized `[T]`, remove legacy mut forms.
2. Typechecker core: unsized restrictions + coercions.
3. Built-ins: `len`/`sizeof` semantics update.
4. VSS integration: dependent field typing and size lowering.
5. Diagnostics + migration fix-its.
6. Docs update.
7. Test expansion and rollout cleanup.
