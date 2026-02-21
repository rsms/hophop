# SLP-3 optional values (`?`) (completed)

## Summary

SLP-3 introduces optional-value types using `?`.

Core intent:

- make nullability explicit in type signatures
- make non-null the default for pointer/reference/slice-like types
- keep semantics simple and C-compatible at runtime

Examples:

```sl
?*T      // nullable pointer to T
?&T      // nullable reference to T
?[T]     // nullable slice view
?mut[T]  // nullable mutable slice view
```

---

## Motivation

Current SLP-2 behavior allows `*`/`&` to be null by default. That keeps migration simple, but type
signatures cannot express intent about whether null is expected.

SLP-3 adds that intent:

- APIs that require a valid reference can say `&T`
- APIs that may receive/return nothing can say `?&T`
- compile-time rules can reject accidental null usage in non-null contexts

---

## Scope

SLP-3 covers:

- syntax and typing of `?` optional values
- conversions/assignability between nullable and non-null forms
- initialization rules for non-null fields in by-value aggregates
- allocation API split between nullable and panic-on-failure forms

SLP-3 does not cover:

- Rust-style borrow/lifetime checking
- full definite-assignment proof engine
- general sum/union option types (`Option[T]`-style enums)

---

## Syntax

### Type grammar extension

```ebnf
Type         = OptionalType | BaseType ;
OptionalType = "?" BaseType ;
BaseType     = MutRefType
             | RefType
             | MutSliceType
             | SliceType
             | PtrType
             | ArrayType
             | DepArrayType
             | TypeName ;
```

### Valid optional targets in SLP-3

In this SLP, `?` is valid on:

- pointers (`?*T`, `?*[T]`, `?*[T N]`)
- references (`?&T`, `?mut&T`, `?&[T N]`, `?mut&[T N]`)
- slices (`?[T]`, `?mut[T]`)

Use on plain value types (e.g. `?i32`) is deferred.

### Null and unwrap spellings (proposed for SLP-3)

- Null literal keyword: `null`
- Unwrap operator: postfix `!`
  - `x!` requires `x : ?T`
  - result type is `T`
  - runtime behavior: panic if `x == null`

Examples:

```sl
var p ?*i32 = null
var q *i32 = p!   // panic if p is null
```

---

## Semantics

- `?X` means value may be absent.
- For pointer/reference/slice families, absence is represented as null.
- Non-optional forms (`X`) are non-null by contract.

This is a type-system contract; runtime representation remains C-like.

---

## Assignability and conversions

Let `T` be a non-optional type.

- `T -> ?T` is implicit.
- `?T -> T` requires explicit unwrap/check.
- `null` literal/type is assignable only to `?T`.
- `null` assignment to `T` is a compile error.

For mutability:

- `mut&T -> &T` remains implicit.
- `mut[T] -> [T]` remains implicit.
- nullable wrappers follow same direction:
  - `?mut&T -> ?&T` implicit
  - `?mut[T] -> ?[T]` implicit

---

## Control-flow typing

Typechecker supports flow narrowing after direct null checks.

Example:

```sl
fn f(x ?&i32) i32 {
    if x == null {
        return 0
    }
    // here x is treated as &i32
    return *x
}
```

Narrowing patterns in SLP-3:

- inside `if x != null { ... }`, `x` narrows from `?T` to `T`
- inside `if x == null { ... } else { ... }`, `x` narrows to `T` in `else`

Narrowing is required only for direct checks against `null` in this SLP.

---

## Initialization rules

When non-null fields exist in by-value aggregates, default-zero initialization is insufficient.

Given:

```sl
struct S {
    r &i32
}
```

Then:

- `var s S` is an error.
- `var s S = {}` is an error.
- `var s S = { r = &x }` is valid.

Arrays of such types:

- `var a [S 2]` is an error.
- `var a [S 2] = {}` is an error.
- fully initialized literals are valid.

Pointer-allocated cases remain allowed:

```sl
var p *S = new(ma, S)
```

Fields may start as null at runtime storage level, but usage rules still require valid assignment
before non-null dereference.

Static analysis for “assigned before use” is optional/future.

---

## Built-in `new` with optionals

Logical signatures once SLP-3 lands:

```sl
// nullable-return forms
fn new(ma mut&MemAllocator, type T) ?*T
fn new(ma mut&MemAllocator, type T, N uint) ?*[T N]

// non-null forms (panic on allocation failure)
fn new(ma mut&MemAllocator, type T) *T
fn new(ma mut&MemAllocator, type T, N uint) *[T N]
```

Dispatch/overload details are backend/typechecker-defined.

---

## Runtime and ABI mapping

For pointer/reference/slice families, optional values use same runtime layouts as SLP-2 with null
as the absent sentinel.

No extra tag word is required in SLP-3 for these families.

---

## Diagnostics

Compiler should produce targeted errors for:

- assigning nullable to non-null without check/unwrap
- using null where non-null is required
- default-initializing by-value aggregates with required non-null fields
- applying `!` to a non-optional type

---

## Migration strategy

Suggested staged rollout:

1. Introduce `?` parsing + typing in permissive mode.
2. Add warnings for implicit nullable-to-non-null flows.
3. Flip to errors in strict/default mode.
4. Update standard APIs (`new`, containers, lookup APIs) to expose nullable intent.

---

## Open questions

1. Should `?` be generalized to all value types in a later SLP?
2. Interaction with `str`:
   - should `?str` be valid as shorthand for `?[u8]` when aliases land?
