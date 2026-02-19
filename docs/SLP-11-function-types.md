# SLP-11 function type syntax

## Summary

SLP-11 adds function types to the type grammar:

```ebnf
FnType = "fn" "(" Params? ")" Result? ;
```

This enables declarations like:

```sl
var callback fn(i32, str) bool
var noResult fn()
```

and, critically for allocator APIs:

```sl
pub struct Allocator {
    impl fn(self mut&Allocator, addr, align, curSize uint, newSizeInOut mut&uint, flags u32) uint
}
```

---

## Motivation

SL currently cannot express function-typed fields, variables, or parameters. This forces ABI hacks
for callback slots (e.g. storing raw integers instead of typed function values).

Adding function type syntax:

- makes callback-oriented APIs first-class
- removes the need for placeholder scalar fields for function pointers
- keeps source-level intent aligned with generated C ABI

---

## Syntax

Add function type as a `Type` form:

```ebnf
Type        = ... | FnType ;
FnType      = "fn" "(" [FnTypeParamList] ")" [Type] ;
FnTypeParamList = FnTypeParamGroup { "," FnTypeParamGroup } ;
FnTypeParamGroup = Ident { "," Ident } Type | Type ;
```

Notes:

- Return type is optional; omitted means `void`.
- Parameter names are optional metadata. Type identity ignores names.
- Parameter runs are supported for consistency with SLP-8:
  - `fn(a, b uint)` is equivalent to `fn(uint, uint)` for type identity.

---

## Semantics

### 1. Type identity

Two function types are equal iff:

- same arity
- pairwise-equal parameter types
- equal return type

Parameter names do not participate in equality.

### 2. Assignability

Initial rule: invariant function types (exact match only).

This keeps behavior simple and avoids introducing variance rules in this SLP.

### 3. Calls

If expression `f` has function type `fn(T1, ..., Tn) R`, then `f(a1, ..., an)` is valid when
arguments are assignable to `T1..Tn`, producing `R`.

### 4. `self` in function types

`self` is not special in function type syntax. It is treated as a normal parameter name.

---

## Examples

```sl
fn apply(x i32, f fn(i32) i32) i32 {
    return f(x)
}

fn add1(x i32) i32 {
    return x + 1
}

fn main() {
    var f fn(i32) i32 = add1
    assert apply(41, f) == 42
}
```

Allocator callback shape:

```sl
pub struct Allocator {
    impl fn(self mut&Allocator, addr, align, curSize uint, newSizeInOut mut&uint, flags u32) uint
}
```

---

## C backend mapping

Function types lower to C function pointers.

Examples:

```sl
var f fn(i32) i32
```

lowers conceptually to:

```c
__sl_i32 (*f)(__sl_i32);
```

Field form:

```sl
impl fn(self mut&Allocator, ...) uint
```

lowers conceptually to:

```c
__sl_uint (*impl)(Allocator* self, ...);
```

---

## Diagnostics

Recommended new/used diagnostics:

- `expected_type` for malformed `fn(...)` type expressions
- `type_mismatch` for assignment/call mismatches involving function types
- `arity_mismatch` for wrong argument count at call sites

---

## Implementation notes

### Parser

- Extend type parsing to accept `SLTok_FN` in `Type`.
- Add `SLAst_TYPE_FN` node kind.
- Parse parameter groups in type context (same group shape as SLP-8), plus optional return type.
- Store canonical type children so downstream passes can compare structural signatures directly.

### Type checker

- Add structural function-type representation (arity + param type ids + return type id).
- Support:
  - variable/field/parameter declarations using function type nodes
  - assignment checks (exact signature match in SLP-11)
  - calling function-typed expressions

### Codegen

- Teach type emission helpers to produce C function-pointer declarators in:
  - field declarations
  - var/const declarations
  - function parameter declarations

---

## Test plan

1. Positive parse/typecheck:
   - `var f fn(i32) i32`
   - `var g fn()`
   - struct field callback type
   - function-typed parameter and call
2. Positive codegen:
   - generated C contains correct function-pointer declarators
3. Negative:
   - malformed `fn(` type syntax
   - arity mismatch on function-typed call
   - type mismatch assigning one function signature to another
4. Regression:
   - existing function declaration syntax and call resolution remain unchanged

---

## Non-goals

SLP-11 does not add:

- closures or lambda literals
- capture semantics
- method receiver semantics in function types
- variance/subtyping rules for function types
- generic function types

