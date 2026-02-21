# SLP-18 reflection (draft)

## Summary

SLP-18 proposes compile-time type reflection for SL.

It introduces:

- `import "std/reflection"`
- a built-in `typeof(...)` form
- type reflection operations such as `.kind()`, `.base()`, `.fields()`

Example sketch:

```sl
import "std/reflection"

type MyInt int
struct Foo { x, y int }

fn main() {
    var x i32
    assert typeof(x) == i32
    assert typeof(i32) == __sl_primtype
    assert typeof(MyInt) == int

    assert i32.kind() == reflection.Kind.Primitive
    assert MyInt.kind() == reflection.Kind.Alias
    assert MyInt.base() == int
    assert Foo.kind() == reflection.Kind.Struct
    assert Foo.fields().len() == 2
}
```

---

## Motivation

SL increasingly needs static introspection over types:

- generic-ish library helpers without full templates
- diagnostics and code-generation helpers
- capability-aware entry wiring with SLP-17 platform contexts

For example, `main` can inspect which context fields are available and choose behavior without
introducing runtime-only host probing.

---

## Goals

- Provide type reflection at compile time with predictable static semantics.
- Keep runtime overhead zero unless metadata is explicitly materialized.
- Support alias/base inspection and aggregate field enumeration.
- Integrate cleanly with current typechecker and SLP-12 context model.

## Non-goals

- Runtime object reflection (`value.field_by_name("x")`).
- Dynamic loading/invocation by symbol name.
- Replacing imports/package resolution with reflection.
- General-purpose macro/template system in this SLP.

---

## Proposed model

### 1. Reflection package

`std/reflection` provides reflection primitives and metadata types.

Draft API:

```sl
pub enum Kind u8 {
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
    typ  __sl_primtype
}
```

`__sl_primtype` is a compiler-provided metatype for type values.

### 2. `typeof(...)`

`typeof` returns type information in constant-expression context.

Draft forms:

- `typeof(expr)` returns the static type of `expr`.
- `typeof(typeExpr)` returns metatype/underlying type according to rules defined below.

### 3. Type-value operations

Operations on type values:

- `T.kind() -> reflection.Kind`
- `T.base() -> __sl_primtype` (valid for alias types; otherwise compile-time error)
- `T.fields() -> [reflection.Field]` (valid for `struct`/`union`; otherwise compile-time error)

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

---

## Interaction with SLP-17 contexts

SLP-17 makes `main` context target-specific (`platform/<target>.Context`).
Reflection provides a way to inspect context shape:

```sl
import "std/reflection"

fn main() {
    const k = typeof(context).kind()
    assert k == reflection.Kind.Struct
}
```

This does not add conditional imports; package selection remains a build-time concern.

---

## Diagnostics (draft)

- `reflect_invalid_operand`: operand is not reflectable in this form
- `reflect_op_not_supported`: operation not valid for this type kind
- `reflect_field_missing`: requested field does not exist
- `reflect_not_const`: reflection expression requires constant-evaluable context

---

## Implementation sketch

1. Add internal metatype representation (`__sl_primtype`) in typechecker.
2. Parse/typecheck `typeof(...)` as a built-in expression form.
3. Add compile-time evaluation path for kind/base/fields.
4. Add `std/reflection` package surface.
5. Add tests for:
   - primitive/alias/struct kind detection
   - alias base extraction
   - aggregate field enumeration
   - invalid reflection operations

---

## Open questions

1. `typeof(TypeName)` exact behavior:
   - Should it always return metatype (`__sl_primtype`)?
   - Should aliases canonicalize (`typeof(MyInt) == int`) as in the sketch?
2. Should `fields()` include promoted embedded fields or only direct declared fields?
3. Should reflection be available everywhere, or only in constant-expression contexts?
4. Should type values be allowed in ordinary runtime expression positions, or restricted to
   reflection built-ins and methods?
