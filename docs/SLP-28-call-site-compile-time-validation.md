# SLP-28 call-site compile-time validation primitives (completed)

## Summary

SLP-28 adds generic language/compiler primitives needed for library-defined, call-site compile-time
validation in SL.

Primary target:
- make APIs like `str.format` validate placeholders/types/count from pure SL code, without
  special typecheck hooks or backend-specific lowering.

Key additions:
1. const-binding visibility for consteval in function bodies
2. const-evaluable loop/index support for `...anytype` packs
3. `const { ... }` statement blocks that must execute at compile time
4. branch specialization for const-evaluable conditions used in type-directed validation
5. const-evaluable string/slice indexing needed for parser-style const blocks (for example,
   `format` string tokenization)

Status:
- implemented
- used by `lib/str/format.sl` to perform placeholder/count/type validation in pure SL
- no `str.format`-specific compiler or backend hooks are required

## Motivation

Current behavior blocks pure-SL call-site validation:

1. `const` parameters are enforced at call sites, but identifiers bound to those parameters are not
   treated as const values inside callee const initializers/consteval checks.
2. `...anytype` indexing requires a const-evaluable integer, but useful patterns like loop indices
   in validation code are not currently accepted.
3. There is no explicit statement-level "must run at compile time" construct for library code.
4. Type-directed branches on `typeof(...)` are not specialized enough to express practical
   validation/dispatch patterns in generic helpers.

As a result, features like SLP-26 were forced into compiler and backend special-cases.

## Goals

- enable library code to run validation logic at compile time at each call site
- keep semantics explicit and deterministic
- avoid introducing runtime boxing/vtables for `...anytype`
- avoid adding runtime dynamic indexing requirements for `...anytype` packs
- avoid feature-specific compiler hardcoding (e.g. no `str.format` special paths)

## Non-goals

- full template/trait system
- runtime dynamic reflection over arbitrary values
- implicit virtual-method dispatch for `...anytype`

## Proposed language changes

### 1. Callee const-binding visibility

Inside function bodies, identifiers bound to:
- `const` parameters
- local `const` declarations whose initializer is const-evaluable

must be usable as const values by const-eval analysis.

This is required for patterns like:

```sl
fn f(const s &str) uint {
    const n = len(s)
    return n
}
```

### 2. `...anytype` pack index const-evaluable rules

For `args ...anytype`:
- `len(args)` remains const-evaluable.
- `args[i]` is valid when `i` is provably const-evaluable in the current const-eval context.
- out-of-bounds const indices remain hard errors.

This must support const-evaluable loop/index patterns used for compile-time validation and
specialized code paths.
No new runtime dynamic indexing behavior is required by this SLP.

### 3. `const { ... }` statement block

Add block syntax:

```sl
const {
    // statements that must run at compile time
}
```

Rules:
- entire block must be const-evaluable
- block is executed during compile-time evaluation
- non-const statements in the block are errors
- diagnostics inside the block are reported at call site when block depends on call arguments

Intended use in APIs:

```sl
fn format(buf *[u8], const pattern &str, args ...anytype) uint {
    const {
        validate_pattern(pattern, args...)
    }
    return format_runtime(buf, pattern, args...)
}
```

### 4. Branch specialization in const-evaluable paths

When conditions are const-evaluable within const-eval execution, non-taken branches are not
required to typecheck under the taken branch's bindings.

Reference behavior in this branch:
- applies in const-evaluated statement contexts
- applies in template-instance function bodies (for `anytype`-driven specialization)
- does not change runtime control-flow semantics; it only changes which branch is required to pass
  static checking for that specialization/evaluation path

This enables practical type-directed generic validation helpers and template branches like:

```sl
fn score(x anytype) i64 {
    if typeof(x) == i64 {
        return x + 1 as i64 - 1 as i64
    } else {
        return len(x) as i64
    }
}
```

### 5. Const-evaluable string/slice indexing for parser logic

Library validation code like `str.format` needs to scan bytes in const strings using loops and
index operations.

Required behavior:
- indexing const-evaluable strings/slices in const-eval execution paths must produce const values
- this must work in nested expressions and loops used by parser-like code

Reference behavior in this branch:
- const-evaluable element indexing (`x[i]`) is supported for const-evaluable string/slice-like
  values used by const-eval execution paths
- nested expressions and loop-driven indices are supported when index values are const-evaluable
- slice-range forms (`x[a:b]`) remain deferred and are not part of this step

## Diagnostics

Add or clarify diagnostics for:
- non-const identifier use in const block
- non-const `...anytype` index in const-evaluable context
- `const` block non-evaluable statements
- `const` block call-site evaluation failure details

## Implementation notes

- Reuse existing const-eval execution engine (`typecheck/consteval.c`, `ctfe_exec.c`) as the
  execution model for `const { ... }`.
- Extend identifier resolution for const-eval to include const-eligible locals/parameters.
- Extend `...anytype` index checking to accept indices proven const-evaluable by the active
  const-eval environment.
- Keep runtime model unchanged; this is compile-time-only behavior.

## Migration plan

1. land SLP-28 primitives (`const { ... }`, const-binding visibility, pack-index consteval support).
2. implement `str.format` call-site validation in pure SL using those primitives.
3. keep runtime formatting/writing logic in SL, but remove/avoid runtime-only validation paths where
   compile-time checks already guarantee correctness.
4. keep format-specific compiler/backend special-cases removed (or remove them if still present in
   branches not yet migrated).

## Test plan

Add tests for:
1. const parameter identifier usable in local const initializer
2. `const { ... }` success and failure paths
3. `...anytype` index with const-evaluable loop/index variables
4. call-site diagnostics originating from const blocks
5. pure-SL format validation helper prototype using these primitives
