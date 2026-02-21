# SLP-6 struct composition (completed)

## Summary

SLP-6 adds struct composition through anonymous embedded struct fields.

Example:

```sl
struct A {
    x int
}

struct B {
    A
    y int
}

struct C {
    B
    z int
}
```

This enables:
- field promotion (`b.x`, `c.y`)
- implicit upcasts along the embedded-base chain (`B -> A`, `C -> B`, `C -> A`)
- zero-cost reference upcasts (`mut&C -> mut&B -> mut&A`)

SLP-6 is intentionally limited to composition of struct types only.

---

## Motivation

Some APIs naturally model "is built from" relationships where one struct is used as the stable
prefix of another. This is useful for:
- capability/value adapters (for example allocator adapters)
- progressive extension of data structures
- reducing boilerplate forwarding code

The feature keeps the language simple while improving ergonomics and reuse.

---

## Syntax

Inside a `struct` body, a field declaration can be either:

1. Named field (existing syntax)
```sl
fieldName Type
```

2. Anonymous embedded field (new)
```sl
TypeName
```

`TypeName` must resolve to a struct type.

Examples:

```sl
struct Vec2 {
    x f32
    y f32
}

struct ColoredVec2 {
    Vec2
    color u32
}
```

---

## Semantic rules

### Embedded base restrictions

- At most one anonymous embedded field is allowed per struct (initial version).
- If present, it must be the first field in the struct.
- The embedded type must be a named struct type (not pointer/reference/slice/array/optional).
- Cycles are forbidden (`A` embeds `A`, or `A -> ... -> A`).

### Promotion and selector resolution

Given:

```sl
struct A { x int }
struct B { A; y int }
struct C { B; z int }
```

- `b.x` resolves to `b.A.x`
- `c.y` resolves to `c.B.y`
- `c.x` resolves to `c.B.A.x`

Resolution order:
1. direct fields of the current struct
2. promoted fields from embedded base, recursively

If more than one candidate is reachable at the same depth, selector is ambiguous and rejected.

### Implicit conversions

For a struct `D` with embedded-base chain `D -> ... -> Base`, implicit conversions are allowed:

- by value: `D -> Base` (projects/copies the base subobject)
- by reference:
  - `&D -> &Base`
  - `mut&D -> mut&Base`
  - `mut&D -> &Base` (via existing mut-to-readonly rule)

No implicit conversion is added for owned pointer types (`*D -> *Base`) in SLP-6.

### Assignability impact

The assignability table is extended with:

- `Derived -> Base` for embedded-base ancestry
- `&Derived -> &Base` for embedded-base ancestry
- `mut&Derived -> mut&Base` for embedded-base ancestry

This is transitive.

### Layout

For:

```sl
struct B {
    A
    y int
}
```

`A` occupies the prefix of `B` at offset `0`. Remaining fields follow normal alignment/layout rules.

This prefix guarantee is required for zero-cost reference upcasts.

---

## Code generation (C11)

A composed struct lowers to a C struct with an explicit first member for the embedded base.

Illustrative lowering:

```sl
struct B {
    A
    y int
}
```

```c
typedef struct B {
    A __sl_base;
    __sl_i32 y;
} B;
```

Lowering examples:
- `accept_A(b)` -> `accept_A(b.__sl_base)`
- `accept_A_ref(&b)` -> `accept_A_ref(&b.__sl_base)`
- `b.x` -> `b.__sl_base.x`

For transitive promotion, codegen follows the resolved selector path.

---

## Examples

### Value and reference upcast

```sl
fn accept_A(v A) {}
fn accept_A_ref(v &A) {}

fn example() {
    var b B
    accept_A(b)       // B -> A
    accept_A_ref(&b)  // &B -> &A
}
```

### Transitive upcast

```sl
fn accept_A(v A) {}
fn accept_B(v B) {}
fn accept_C(v C) {}

fn example() {
    var c C
    accept_A(c) // C -> A
    accept_B(c) // C -> B
    accept_C(c)
}
```

---

## Diagnostics

Recommended new diagnostics:

- `embedded_field_not_first`: anonymous embedded field must be first
- `multiple_embedded_fields`: only one anonymous embedded field allowed
- `embedded_type_not_struct`: embedded field type must be a struct
- `embedded_cycle`: embedded struct cycle detected
- `ambiguous_promoted_field`: selector is ambiguous through embedded bases

---

## Non-goals

SLP-6 does not add:
- interfaces/trait objects
- multiple anonymous embedded bases
- method declarations or method sets
- owned-pointer upcast rules (`*Derived -> *Base`)
