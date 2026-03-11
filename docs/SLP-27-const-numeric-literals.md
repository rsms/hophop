# SLP-27 constant numeric literal types (completed)

## Summary

SLP-27 introduces `const_int` and `const_float` as constant numeric types and makes them the
default types of numeric literals:

- integer literals default to `const_int`
- float literals default to `const_float`
- rune literals default to `const_int`

This enables reusable compile-time numeric constants that can flow into concrete numeric
destinations without forcing immediate casts.

Example:

```sl
const MessageIDBase = 100

fn main() {
    var msgid u8 = MessageIDBase + 1
    assert msgid == 101 as u8
}
```

## Motivation

Current defaults (`int`/`f64`/`rune`) work for many runtime cases but are less ergonomic for
compile-time constants reused across multiple numeric destination types.

Constant numeric types make intent explicit:

- values stay in const-evaluable space by default
- conversion to concrete runtime numeric storage happens at assignment/use boundaries

## Syntax

Add built-in type names:

- `const_int`
- `const_float`

No declaration syntax changes are required.

## Semantic rules

### 1. Literal default types

Default literal types become:

- integer literal -> `const_int`
- float literal -> `const_float`
- rune literal -> `const_int`

### 2. `const_int` conversion rules

`const_int` is implicitly convertible to any integer destination type when the value is in range.

Out-of-range conversions are type errors.

`const_int` is also implicitly convertible to floating-point destination types when the integer
value fits in destination mantissa precision:

- `f32`: up to 23 bits of integer precision
- `f64`: up to 53 bits of integer precision

If a `const_int` value exceeds destination integer precision, implicit conversion is a type error.
Explicit casts remain available and may lose precision per cast semantics.

### 3. `const_float` conversion rules

`const_float` is implicitly convertible to any floating-point destination type.

When precision/range cannot be represented in destination type, conversion is a type error.

`const_float` is not implicitly convertible to integer destination types.
Integer conversion requires an explicit cast.

### 4. Inference concretization for mutable runtime storage

Inference for mutable runtime storage remains practical:

- `var x = 1` infers `int`
- `var y = 1.0` infers `f64`

while const declarations preserve constant numeric behavior:

- `const A = 1` remains const numeric and participates in implicit integer conversion on use
- `const B = 1.0` remains const numeric and participates in implicit float conversion on use

### 5. Arithmetic on constant numeric values

Constant-only arithmetic expressions remain const-evaluable and produce constant numeric results
(`const_int` / `const_float`) according to existing numeric operator rules.

### 6. Position restrictions for `const_int` and `const_float`

`const_int` and `const_float` are compile-time numeric types and are restricted to these explicit
type positions:

- function return types
- function parameters only when the parameter is marked `const` (for example `fn f(const x const_int)`)
- `const` declaration explicit types (local or top-level)
- `as` cast targets (`x as const_float`)

They are not valid as mutable runtime storage types (for example `var x const_int`).

## Diagnostics

- keep numeric range diagnostics explicit for implicit `const_int` conversions
- keep precision/range diagnostics explicit for implicit `const_float` conversions
- add a targeted diagnostic when a parameter uses `const_int`/`const_float` without the `const`
  parameter modifier

## Compatibility and migration

### Source compatibility

Most code should continue to work with existing implicit conversion paths.

Potential impact areas:

- overload resolution where literal default-type identity materially affects candidate ranking
- edge cases relying on previous literal defaulting before inference concretization

### Migration guidance

When ambiguity appears, use explicit casts at the call/assignment site.

## Implementation notes

Typechecker:

- extend type model with `const_int` and `const_float`
- update literal typing for int/float/rune AST nodes
- update assignability/conversion logic for const numeric conversions
- update inference concretization so mutable `var` defaults remain `int`/`f64`

Spec/docs:

- add the two built-ins to built-in type lists
- update literal typing and inference sections accordingly

## Test plan

Add/update tests for:

1. `const_int` behavior:
   - implicit conversion to `u8`, `i32`, `uint`, etc. with in-range success
   - out-of-range conversion failure
   - rune literal defaulting to `const_int` and conversion checks
2. `const_float` behavior:
   - implicit conversion to `f32`/`f64`
   - precision/range failure diagnostics where applicable
3. Inference behavior:
   - `var x = 1` -> `int`
   - `var y = 1.0` -> `f64`
4. Const declarations using numeric literals in mixed integer/float destination contexts

## Non-goals

SLP-27 does not add:

- full arbitrary-precision runtime numeric types
- generic numeric constraint/trait systems
- automatic replacement of explicit cast APIs

## Resolved decisions

1. Implicit `const_int -> float` conversion is allowed with precision bounds.
   - allowed when the integer value fits destination mantissa precision (`f32`: 23 bits, `f64`:
     53 bits)
   - otherwise implicit conversion is a compile-time type error
   - explicit casts are still allowed and may lose precision
2. `const_float -> integer` remains explicit-only.
   - no implicit conversion, even for integral-valued constants
   - callers must use an explicit cast
3. `const_int`/`const_float` explicit type positions are restricted.
   - allowed only in function returns, `const` parameters, `const` declarations, and `as` targets
   - not allowed for mutable runtime storage types like `var x const_int`
