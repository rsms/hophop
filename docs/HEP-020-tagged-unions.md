# HEP-20 tagged unions via enum payload variants (completed)

## Summary

HEP-20 extends `enum` so each variant may optionally carry a payload type.

```hop
enum Shape u8 {
    Point
    Circle f64
    Rectangle struct {
        width  f64
        height f64
    }
}
```

This keeps enum tags while adding per-variant data, enabling algebraic-data-type style modeling.

HEP-20 also proposes `switch` case variant patterns with narrowing and alias binding:

```hop
switch shape {
    case Shape.Point { ... }
    case Shape.Circle as c { ... }
    case Shape.Rectangle as r { ... }
}
```

## Motivation

Current HopHop enums are tag-only. Many real models need tagged choices with data:

- protocol/event message variants
- AST nodes
- success/failure values with different payload shapes

Today these require parallel structs + manual tagging, which is verbose and weakly coupled.

## Syntax

### 1. Enum declarations

Existing enum declarations are extended so each variant can include an optional payload type.

```ebnf
EnumDecl            = "enum" Ident Type "{" [ EnumItemList ] "}" .
EnumItemList        = EnumItem { FieldSep EnumItem } [ FieldSep ] .
EnumItem            = Ident [ EnumPayload ] [ EnumTagInit ] .
EnumPayload         = Type .
EnumTagInit         = "=" Expr .
```

Anonymous struct payloads use normal type syntax: `Variant struct { ... }`. The old
`Variant{ ... }` declaration form is invalid.

### 2. Variant patterns in switch cases

`case` labels are extended with variant-pattern forms.

```ebnf
CaseClause          = "case" CasePattern { "," CasePattern } Block .
CasePattern         = Expr | VariantPattern .
VariantPattern      = TypeName "." Ident [ "as" Ident ] .
```

Notes:

- `case Shape.Circle` remains valid as a pattern even without aliasing.
- `case Shape.Circle as c` introduces a narrowed alias.

## Examples

### 1. Narrowing on the switched identifier

```hop
fn area(shape &Shape) f64 {
    switch shape {
        case Shape.Point {
            return 0.0
        }
        case Shape.Circle {
            return math.PI * shape.radius * shape.radius
        }
        case Shape.Rectangle {
            return shape.width * shape.height
        }
    }
}
```

### 2. Narrowed alias

```hop
fn area(shape &Shape) f64 {
    switch shape {
        case Shape.Point { return 0.0 }
        case Shape.Circle as c { return math.PI * c.radius * c.radius }
        case Shape.Rectangle as r { return r.width * r.height }
    }
}
```

### 3. Payload construction

```hop
var c = Shape.Circle(2.0)
var r = Shape.Rectangle{ width: 3.0, height: 4.0 }
var p = Shape.Point
```

## Semantics

### 1. Variant model

- Each enum variant has a discriminant tag of the enum base integer type.
- A variant may have no payload (tag-only) or one payload type.
- Struct payload field names must be unique within that struct payload type.
- Mixed payload and non-payload variants are allowed in one enum.

### 2. Tags and zero-initialization

- Existing enum tag rules remain: tags can be implicit or explicit (`= Expr`), including payload
  variants (`Variant PayloadType = n` is legal).
- `var v Enum` uses normal zero-initialization rules.
- For enums, zero-initialization is valid only if one variant has tag value `0`; otherwise typed
  zero-initialization of that enum is a compile error.

### 3. Construction

- Tag-only variant value: `Enum.Variant`.
- Struct payload variant value: `Enum.Variant{ field: expr, ... }`.
- Scalar payload variant value: `Enum.Variant(value)`.
- Tuple payload variant value: `Enum.Variant(a, b, ...)`.
- Struct payload constructor syntax/semantics follow struct literal behavior:
  - named-field initialization
  - omitted fields are allowed and follow struct rules (declaration defaults where present,
    otherwise zero-value)
- If HEP-16 contextual enum literals are enabled, contextual payload constructors may be allowed
  where expected enum type context is clear; otherwise `Enum.Variant...` is required.

### 4. Comparison and ordering

- Equality for payload enums compares tag and payload value (whole-value equality), not tag-only.
- Ordering for payload enums compares by tag value only.
- Programs may customize ordering through comparison hooks (for example
  `fn __order(a, b &Shape) int`).

### 5. Switch pattern typing

- Variant patterns are valid only in expression-switch (`switch expr`) where `expr` has enum type.
- `case Enum.V` matches when subject tag equals `V`.
- `case Enum.V as name` binds `name` to the payload type of variant `V`; it is invalid for
  no-payload variants.
- Multi-pattern cases are supported (`case Enum.A, Enum.B { ... }` and aliases per pattern).

### 6. Narrowing and aliasing

- Variant narrowing is introduced only by `switch` variant patterns.
- Equality tests like `if x == Enum.V` are plain boolean checks and do not narrow.
- In `case Enum.V` body, if switch subject is an identifier, that identifier is narrowed to `V`.
- In multi-pattern case clauses (for example `case Enum.A, Enum.B { ... }`), the switched
  identifier itself is not narrowed in the case body.
- In `case Enum.V as n`, `n` is always available as narrowed alias, regardless of subject form.
- In multi-pattern clauses with aliases, each alias is typed according to its own pattern; aliases
  are explicit variant-view bindings and programmers must use them carefully.
- For non-identifier switch subjects (for example `switch getShape()`), payload access requires
  alias form (`case Shape.Circle as c { ... }`) or pre-binding the subject to a local before switch.
- Alias semantics are sugar for introducing a case-local temporary of the switched value; this is an
  aliasing view, not a required copy.

Canonical form:

```hop
fn f(t &Token) {
    switch t {
        case Token.Int { var _ i64 = t.value }
        default {}
    }
}
```

### 7. Scope and shadowing

- `switch` introduces a new scope.
- `as` aliases follow normal local-definition shadowing rules:
  - shadowing outer locals is allowed
  - duplicate names in the same scope are invalid

### 8. Exhaustiveness

- Expression-switch over finite-domain subjects must be exhaustive unless a `default` clause
  exists.
- Finite-domain subjects in this proposal are:
  - enum types (payload and non-payload)
  - `bool`
- Exhaustive coverage rules:
  - enum subject: every declared variant appears in at least one case label
  - `bool` subject: both `true` and `false` appear in case labels
- For subject types without a known finite value set (for example `int`), exhaustive checking is
  not required.

### 9. Runtime representation

Tagged-union layout is part of this proposal contract:

- representation is `struct { tag TagType; payload union { ... } }`
- payload union members are per-variant payload types
- backend must provide deterministic naming/mapping for these emitted components

## Diagnostics

Proposed diagnostics:

- `enum_variant_payload_field_duplicate`: duplicate field in an anonymous struct payload
- `enum_variant_unknown`: unknown enum variant
- `switch_variant_pattern_non_enum_subject`: variant pattern requires enum switch subject
- `switch_variant_pattern_bind_conflict`: invalid duplicate alias in same scope
- `selector_variant_field_outside_narrow`: variant payload field access requires narrowed value
- `switch_non_exhaustive_finite_domain`: finite-domain switch missing required cases and no `default`
- `enum_zero_init_missing_tag0`: typed zero-initialization invalid because enum has no tag-0 variant

Fallback to existing `ParseError`/`NameResolutionError`/`TypeError` classes is acceptable.

## Compatibility and migration

Source compatibility:

- Existing enums without payload are unchanged.
- Existing `case Enum.V` remains valid.
- `switch` over finite-domain subjects (`enum`, `bool`) becomes stricter due to exhaustiveness
  requirement when `default` is absent.

Migration path for manual tagged-struct patterns:

1. Replace manual tag + parallel payload structs with one payload enum.
2. Replace tag checks with variant patterns in switch.
3. Move payload field accesses into narrowed cases.

## Implementation notes

### Parser

- Extend enum item parsing for optional payload type between variant name and optional `= Expr`.
- Extend `case` parsing to recognize variant pattern forms plus optional `as` aliasing.

### Typechecker

- Represent enum variants with optional payload shape metadata.
- Type-check payload constructors (`Enum.V{...}` for struct payloads, `Enum.V(...)` for non-struct
  payloads).
- Apply struct-literal omission/default rules to struct payload constructor fields.
- Reject typed zero-initialization when enum has no tag-0 variant.
- Implement payload-enum equality as tag+payload and ordering as tag-only.
- Add switch-case narrowing environment for matched variant.
- Validate pattern bindings, shadowing, and selector legality under narrowing.
- Enforce finite-domain-switch exhaustiveness (`enum`, `bool`) unless `default` is present.

### C backend

- Lower payload enums to tagged struct + payload union layout.
- Lower variant matches to tag comparisons.
- Lower narrowed selectors/aliases to selected payload-union member projections.
- Emit/stabilize payload-enum layout as `struct { tag; union payload; }` contract.

## Test plan

1. Positive:
   - enum with mixed payload and payload-less variants
   - `case Enum.V` narrowing with direct subject selectors
   - `case Enum.V as x` alias usage
   - multi-pattern case with and without aliases
   - payload constructor expressions
   - payload constructor with omitted fields/defaults
   - exhaustive enum switch without `default`
   - exhaustive `bool` switch without `default`
2. Negative:
   - payload field duplicates
   - accessing payload field outside narrowed scope
   - unknown variant in case
   - duplicate alias binding in case pattern
   - non-exhaustive enum switch without `default`
   - non-exhaustive `bool` switch without `default`
   - typed zero-init of enum lacking tag `0`
3. Codegen:
   - generated C for payload enums compiles
   - tag dispatch + payload reads/writes behave correctly
   - emitted layout includes tag + payload union contract

## Non-goals

- Match guards (`case P if cond`).
- Variant payload destructuring in `case` labels.
- Standalone `match` expression form.
