# HEP-18 reflection (completed)

## Summary

HEP-18 proposes compile-time type reflection for HopHop.

Current status in `Reference-hop` (phase 1 building blocks):

- builtin metatype `type` exists
- type names are valid value expressions of type `type`
- `typeof(expr)` exists and returns a `type` value
- `type` values support `==` / `!=`
- builtin `kind(t)` / `t.kind()` exists (returns builtin `TypeKind`, fallback `u8` if builtin packages are unavailable)
- builtin `base(t)` / `t.base()` exists for alias type values
- builtin `is_alias(t)` / `t.is_alias()` exists (returns `bool`)
- builtin `is_const(x)` exists for call-site constness checks
- builtin `type_name(t)` / `t.type_name()` exists (returns `&str`)
- `typeof(TypeName)` currently evaluates to `type` (metatype of type-values)
- `TypeKind` enum builtin surface exists; `fields()` is not implemented yet

It introduces:

- a built-in `typeof(...)` form
- type reflection operations such as `.kind()`, `.base()`, `.fields()`

Example sketch:

```hop
import "builtin"

type MyInt int
struct Foo { x, y int }

fn main() {
    var x i32
    assert typeof(x) == i32
    assert typeof(i32) == __hop_primtype
    assert typeof(MyInt) == int

    assert i32.kind() == builtin.TypeKind.Primitive
    assert MyInt.kind() == builtin.TypeKind.Alias
    assert MyInt.base() == int
    assert Foo.kind() == builtin.TypeKind.Struct
    assert Foo.fields().len() == 2
}
```

---

## Motivation

HopHop increasingly needs static introspection over types:

- generic-ish library helpers without full templates
- diagnostics and code-generation helpers
- capability-aware entry wiring with HEP-17 platform contexts

For example, `main` can inspect which context fields are available and choose behavior without
introducing runtime-only host probing.

---

## Goals

- Provide type reflection at compile time with predictable static semantics.
- Keep runtime overhead zero unless metadata is explicitly materialized.
- Support alias/base inspection and aggregate field enumeration.
- Integrate cleanly with current typechecker and HEP-12 context model.

## Non-goals

- Runtime object reflection (`value.field_by_name("x")`).
- Dynamic loading/invocation by symbol name.
- Replacing imports/package resolution with reflection.
- General-purpose macro/template system in this HEP.

---

## Proposed model

### 1. Reflection builtins

The builtin surface provides reflection primitives and metadata types.

Draft API:

```hop
pub enum TypeKind u8 {
    Invalid
    Primitive
    Alias
    Struct
    Union
    Enum
    Pointer
    Reference
    Slice
    Array
    Optional
    Function
}

pub struct Field {
    name str
    typ  __hop_primtype
}
```

`__hop_primtype` is a compiler-provided metatype for type values.

### 2. `typeof(...)`

`typeof` returns type information in constant-expression context.

Draft forms:

- `typeof(expr)` returns the static type of `expr`.
- `typeof(typeExpr)` returns metatype/underlying type according to rules defined below.

### 3. Type-value operations

Operations on type values:

- `T.kind() -> TypeKind`
- `T.base() -> __hop_primtype` (valid for alias types; otherwise compile-time error)
- `T.type_name() -> &str`
- `T.fields() -> [Field]` (valid for `struct`/`union`; otherwise compile-time error)
- `is_const(x) -> bool`
- `ptr(T) -> __hop_primtype`
- `slice(T) -> __hop_primtype`
- `array(T, N) -> __hop_primtype`

### 4. Equality

Type-value equality (`==`, `!=`) compares semantic type identity.

Alias equality details are an open design point (see Open questions).

---

## Semantics

1. Reflection expressions are compile-time evaluable.
2. Invalid operation/shape combinations are compile-time errors.
3. No runtime metadata table is required for operations that are fully compile-time.
4. If reflection results are lowered into runtime values (for example materialized field lists),
   codegen emits the minimum required static data.
5. `is_const(x)` is intended for `const { ... }` validation and specialization. It reports
   whether the operand supplied at the current call site is const-evaluable.

---

## Interaction with HEP-17 contexts

HEP-17 makes `main` context target-specific (`platform/<target>.Context`).
Reflection provides a way to inspect context shape:

```hop
import "builtin"

fn main() {
    const k = typeof(context).kind()
    assert k == builtin.TypeKind.Struct
}
```

This does not add conditional imports; package selection remains a build-time concern.

---

## Diagnostics

- `reflect_invalid_operand`: operand is not reflectable in this form
- `reflect_op_not_supported`: operation not valid for this type kind
- `reflect_field_missing`: requested field does not exist
- `reflect_not_const`: reflection expression requires constant-evaluable context

---

## Implementation sketch

1. Add internal metatype representation (`__hop_primtype`) in typechecker.
2. Parse/typecheck `typeof(...)` as a built-in expression form.
3. Add compile-time evaluation path for kind/base/fields.
4. Add reflection builtin surface.
5. Add tests for:
   - primitive/alias/struct kind detection
   - alias base extraction
   - aggregate field enumeration
   - invalid reflection operations

---

## Open questions

1. `typeof(TypeName)` exact behavior:
   - Should it always return metatype (`__hop_primtype`)?
   - Should aliases canonicalize (`typeof(MyInt) == int`) as in the sketch?
2. Should `fields()` include promoted embedded fields or only direct declared fields?
3. Should reflection be available everywhere, or only in constant-expression contexts?
4. Should type values be allowed in ordinary runtime expression positions, or restricted to
   reflection built-ins and methods?
