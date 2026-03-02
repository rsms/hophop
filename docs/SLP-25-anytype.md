# SLP-25 anytype parameters and variadic packs (completed)

## Summary

SLP-25 introduces `anytype` as a function-parameter placeholder for unconstrained argument types.

This includes variadic use:

```sl
fn something(arg anytype)
fn collect(args ...anytype)
```

`anytype` is intended for compile-time-guided, library-level polymorphism without introducing a
full general template system.

## Motivation

SL currently has no way to express APIs that accept heterogeneous argument types while preserving
static type information of each argument.

`anytype` enables these APIs with explicit signature-level syntax.

## Syntax

### Type grammar

Add:

```ebnf
AnyType = "anytype" .
Type    = ... | AnyType .
```

### Position restrictions

In this SLP, `anytype` is only valid in function parameter positions:

- function declarations (`fn ...` parameter lists)
- function types (`fn(...)` parameter lists)

It is invalid in:

- variable declarations
- field declarations
- return types
- type aliases as standalone data types

## Semantic rules

### 1. Non-variadic `anytype`

Each `anytype` parameter slot is bound to the static type of the corresponding argument at the call
site.

Inside the function body, uses of that parameter are typechecked against that bound concrete type.

### 2. Variadic `...anytype` is a heterogeneous pack

`...anytype` binds a compile-time argument pack, not a homogeneous slice.

- tail arguments may have different types
- the pack has no required runtime contiguous representation

### 3. Pack operations (for `...anytype` bindings)

For parameter `args ...anytype`:

- `len(args)` is valid and const-evaluable
- `args[i]` is valid only when index expression `i` is a const-evaluable integer in bounds
- `typeof(args[i])` is valid and const-evaluable

Indexing a pack with non-const-evaluable index is a type error.
This allows consteval-driven iteration patterns where the index variable remains const-evaluable.

### 4. Spread behavior

For this SLP:

- array/slice spread into `...anytype` is invalid and out of scope
- pack forwarding `args...` from one `...anytype` function to another `...anytype` function is
  valid

### 5. Interaction with `const` parameters (SLP-24)

`anytype` does not imply const-evaluable argument values by itself.

- `x anytype`: any argument type/value accepted
- `const x anytype`: argument must be const-evaluable
- `const args ...anytype`: each pack element must be const-evaluable

### 6. Overload and type identity

- `anytype` participates in function signature identity
- `fn(anytype)` is distinct from `fn(i32)`
- overload resolution treats `anytype` parameters as unconstrained type matches, then applies
  normal ranking and any `const` constraints

## Diagnostics

Recommended new diagnostics:

- `anytype_invalid_position`: `anytype` is not allowed here
- `anytype_pack_index_not_const`: pack index must be const-evaluable
- `anytype_pack_index_oob`: pack index out of bounds
- `anytype_spread_requires_pack`: spread into `...anytype` requires an `anytype` pack value

## Compatibility and migration

Backward-compatible: existing programs without `anytype` are unchanged.

`anytype` becomes a reserved keyword/type token and can no longer be used as an identifier.

## Implementation notes

Parser:

- recognize `anytype` token in type grammar
- enforce allowed-position checks

Typechecker:

- represent non-variadic `anytype` parameter slots as per-call bound type variables
- represent `...anytype` as typed pack bindings
- extend expression typing for `len(pack)` and `pack[index]` with const-index requirement
- include `anytype` in function-type identity and overload matching

Codegen:

- monomorphize per concrete call-shape/type vector, or use equivalent lowering preserving static semantics

## Test plan

Add tests for:

1. Positive:
   - fixed `anytype` parameter with multiple concrete call types
   - `...anytype` call with heterogeneous arguments
   - `len(args)` and const-index `args[i]`
   - consteval loop-index access into `args` where index expression is const-evaluable
   - forwarding `args...` between `...anytype` functions
2. Negative:
   - `anytype` in local var/field/return positions
   - non-const pack index
   - out-of-bounds const pack index
   - slice spread into `...anytype`
3. Type identity:
   - mismatch between function types with and without `anytype`

## Non-goals

SLP-25 does not add:

- full generic constraints or trait systems
- arbitrary runtime reflection over all values
- implicit runtime boxing of heterogeneous variadics

## Resolved decisions

1. `args[i]` on `...anytype` is limited to const-evaluable index expressions in this SLP.
   - runtime indexing is out of scope for now.
   - consteval iteration patterns such as `for var i uint = 0; i < len(args); i++ { args[i] }`
     are intended to be supported when `i` is const-evaluable.
2. Array/slice spread into `...anytype` is not planned in this SLP.
   - forwarding `...anytype` packs (`a(args...)`) is supported.
