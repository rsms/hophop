# SLP-33 rawptr (completed)

## Summary

SLP-33 introduces `rawptr`, a built-in opaque pointer type for "pointer to anything".

`rawptr` exists for cases where SL needs to carry an address without exposing pointee type
information in the static type system.

Example:

```sl
var opaque1 rawptr
var opaque2 rawptr = null
assert opaque1 == opaque2

var i = 123
var ip1 *int = &i
var opaque3 rawptr = ip1 as rawptr
var ip2 *int = opaque3 as *int
assert ip2 == ip1

opaque3 = null
```

This SLP is a backfilled proposal: the implementation landed before the proposal text.
The document records the implemented behavior and intent.

## Motivation

Today some runtime-facing APIs need to carry opaque addresses:

- allocator implementations
- platform glue
- arena/block bookkeeping
- foreign/runtime handles that are pointer-shaped but intentionally untyped

Before `rawptr`, these cases used integer types such as `uint` as a stand-in for "opaque
pointer". That has a few problems:

- it hides pointer intent
- it makes null/default semantics less direct
- it permits arithmetic and numeric casts that are not part of the intended model
- it forces library code to encode "no pointer" as `0` instead of `null`

`rawptr` fixes that by giving the language a dedicated opaque-pointer type with explicit
conversion boundaries.

## Goals

- Add a built-in opaque pointer type.
- Make the zero/default value `null`.
- Require explicit casts at typed-pointer boundaries.
- Replace stdlib uses of integer-as-pointer placeholders with `rawptr`.
- Keep the feature small and mechanical.

## Non-goals

SLP-33 does not add:

- pointer arithmetic on `rawptr`
- dereference or indexing on `rawptr`
- implicit conversions between `rawptr` and typed pointers/references
- casts from `rawptr` to integers or other non-pointer types
- a generic `void*`-style escape hatch for all conversion rules

## Syntax

Add built-in type name:

- `rawptr`

No new expression syntax is added.

## Semantic rules

### 1. Nature of `rawptr`

`rawptr` is a built-in pointer-like type with no pointee type information.

It may hold:

- `null`
- a value explicitly cast from a pointer type
- a value explicitly cast from a reference type
- another `rawptr`

### 2. Zero value

The zero/default value of `rawptr` is `null`.

```sl
var p rawptr
assert p == null
```

### 3. Assignability

`rawptr` accepts:

- `rawptr`
- `null`

`rawptr` does not implicitly accept:

- `*T`
- `&T`
- other non-pointer types

Examples:

```sl
var p rawptr = null
var q rawptr = p
```

```sl
var x i32 = 1
var px *i32 = &x
var bad rawptr = px // invalid
```

### 4. Casts to `rawptr`

Conversion into `rawptr` is explicit except for `null` and same-type assignment.

Allowed:

- `ptr as rawptr` where `ptr : *T`
- `ref as rawptr` where `ref : &T`
- `rp as rawptr` where `rp : rawptr`
- `null as rawptr`

### 5. Casts from `rawptr`

Getting a useful typed pointer/reference back from `rawptr` requires an explicit cast.

Allowed:

- `rp as *T`
- `rp as &T`
- `rp as rawptr`

Disallowed:

- `rp as int`
- `rp as uint`
- `rp as bool`
- `rp as T` for non-pointer/non-reference `T`

### 6. Comparison

`rawptr` is comparable and ordered.

Operationally:

- `rawptr == rawptr` uses pointer identity
- `rawptr != rawptr` uses pointer identity
- `rawptr` compares with `null`
- ordering uses pointer-address order

This matches the existing pointer/reference comparison model.

### 7. Type inference

`var x = null` remains invalid because `null` does not determine a concrete type.

`rawptr` participates only when explicitly named or explicitly cast to.

### 8. Runtime/backend model

Backend intent:

- C backend: lower `rawptr` to `void*`
- evaluator/MIR/Wasm: treat `rawptr` as pointer-like opaque storage

This is a representation detail, not source-level subtyping.

## Standard-library impact

The primary stdlib migration in SLP-33 is allocator-facing opaque addresses.

Before:

```sl
impl fn(*Allocator, uint, uint, uint, *uint, u32) uint
```

After:

```sl
impl fn(*Allocator, rawptr, uint, uint, *uint, u32) rawptr
```

This lets allocator code use `null` instead of integer sentinel values and makes address-shaped
APIs explicit.

## Compatibility and migration

Source patterns using integer-as-opaque-pointer should migrate to `rawptr`.

Typical updates:

1. replace `uint` fields/params/results that really mean opaque address with `rawptr`
2. replace `0` sentinel checks with `null`
3. add explicit casts at typed-pointer boundaries

Example:

```sl
var addr rawptr = typed_ptr as rawptr
var typed *T = addr as *T
```

## Diagnostics

Recommended diagnostics:

- implicit pointer/reference to `rawptr` assignment: type mismatch
- implicit `rawptr` to typed pointer/reference assignment: type mismatch
- `rawptr as NonPointerType`: targeted cast diagnostic

Example diagnostic shape:

```text
type mismatch: cannot cast rawptr to int
```

## Implementation notes

Typechecker:

- add builtin type `rawptr`
- permit `null` assignment to `rawptr`
- permit `rawptr == null` / `null == rawptr`
- gate casts so `rawptr` participates only in the explicit pointer/reference paths above

Evaluator and MIR:

- zero-init `rawptr` to `null`
- preserve explicit cast behavior at const-eval/runtime-eval layers
- treat `rawptr` as pointer-like for equality/order

Codegen:

- lower `rawptr` to backend-native opaque pointer representation
- keep typed-pointer recovery explicit at source level only

Library:

- migrate allocator and arena APIs away from integer placeholder addresses

## Test plan

Add/update tests for:

1. Positive:
   - `var p rawptr` zero-inits to `null`
   - `var p rawptr = null`
   - `*T as rawptr`
   - `rawptr as *T`
   - `rawptr == null`
2. Negative:
   - implicit `*T -> rawptr`
   - implicit `rawptr -> *T`
   - `rawptr as int`
3. Library/backend:
   - allocator callback signatures using `rawptr`
   - Wasm/C backend coverage for custom allocator paths

## Future work (separate SLPs)

- typed opaque-handle wrappers in the standard library
- explicit foreign/extern pointer interop rules
- any future pointer provenance model, if the language grows one
