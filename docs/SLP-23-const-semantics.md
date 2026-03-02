# SLP-23 const semantics (draft)

## Summary

SLP-23 proposes changing `const name = val` to mean compile-time constant in all scopes
(top-level and local), not only top-level.

This proposal focuses on const-evaluable initializers and const immutability consistency.

Numeric literal default-type changes (`const_int` / `const_float`) are split into SLP-27.

## Current behavior (Reference-slc, git 0020658990470fd1a9e7f957049a0cef4814b511)

Observed and verified behavior today:

1. Top-level `const` initializers are required to be const-evaluable.
   - enforced by `SLTCValidateTopLevelConstEvaluable` in typecheck pipeline
   - failures use `SL2055: const initializer must be a const expression`
2. Local `const` declarations currently only require an initializer and type compatibility.
   - no general "initializer must be const-evaluable" validation for local const declarations
3. Assignments to `const` bindings are not consistently rejected during typecheck in normal
   (non-consteval) paths.
   - this can pass `slc check`
   - generated C uses `const` storage and may fail during C compilation later when assigned
4. In consteval execution, `const` bindings are tracked as immutable and assignment is rejected.

So `const` semantics are currently mixed: compile-time-constant enforcement is strong at top-level,
but weaker for local declarations and assignment diagnostics in ordinary typechecking paths.

## Motivation

`const` should mean one thing everywhere: compile-time constant value.

Current split behavior is surprising:

- code can pass `slc check` and fail later at C compile due to const assignment
- top-level/local const semantics differ

Unifying semantics removes this inconsistency and improves diagnostic quality.

## Syntax

No syntax changes.

`const` declaration syntax remains:

```sl
const Name = Expr
const Name Type = Expr
const A, B = Expr1, Expr2
```

## Semantic rules

### 1. Const declarations are compile-time in all scopes

For any `const` declaration (top-level or local), every initializer expression bound to each const
name must be const-evaluable.

If any initializer is not const-evaluable, typecheck fails.

### 2. Const bindings are immutable in all scopes

A const binding is not assignable.

Any direct assignment/compound assignment targeting a const binding is a type error.

### 3. Type-position usage remains supported

Existing behavior where top-level const names can resolve to type values in type positions remains:

```sl
const T = i32
var x T
```

No regression from current [TYPE-CONSTR-007]/[TYPE-CONSTR-008] intent.

## Diagnostics

- Reuse `SL2055` (`const initializer must be a const expression`) for non-const-evaluable const
  initializers in all scopes.
- Reuse existing assignment/type-mismatch diagnostics for assignment-to-const, with const-specific
  diagnostic detail text.
- For grouped const declarations, emit diagnostics at each failing initializer expression.

## Compatibility and migration

### Source compatibility

Potentially breaking changes:

1. Local const initializers that are not const-evaluable will now fail.
2. Assignments to const bindings that currently pass `slc check` will fail at typecheck (earlier,
   better diagnostic).

### Behavioral improvements

- Removes check-vs-compile mismatch for const assignment.
- Makes top-level and local const behavior consistent.

## Implementation notes

Typechecker:

- apply const-evaluable initializer validation to local const declarations
- make assignment-target analysis aware of const bindings

Consteval:

- keep existing immutability behavior for const bindings
- align non-consteval typechecker behavior with consteval assignment restrictions

Codegen:

- no special fallback reliance on C compile errors for const assignment
- preserve current const lowering once typechecker enforces semantics

## Test plan

Add/update tests for:

1. Local const initializer const-evaluable requirement:
   - non-const local initializer rejected
   - const-evaluable local initializer accepted
2. Assignment to const bindings:
   - local const assignment rejected at typecheck
   - top-level const assignment rejected at typecheck
3. Existing top-level const behavior preserved:
   - non-const top-level initializer still rejected
4. Grouped const diagnostics:
   - diagnostics are emitted per failing initializer, not only at declaration-level

## Non-goals

SLP-23 does not add:

- numeric literal default-type changes
- new numeric type categories
- new mutability qualifiers for non-const bindings

## Resolved decisions

1. Assignment-to-const reuses existing assignment/type-mismatch diagnostics, with const-specific
   detail text.
2. Grouped const declarations diagnose each failing initializer expression individually.
   - Example: `const a, b = x, y` with both `x` and `y` non-const-evaluable emits two errors.
