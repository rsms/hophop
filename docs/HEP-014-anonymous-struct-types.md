# HEP-14 anonymous aggregate types (struct/union) (completed)

## Summary

HEP-14 adds inline anonymous aggregate type syntax:

```hop
{ mem Allocator; fs ReadFS }              // anonymous struct (`struct` keyword omitted)
struct { mem Allocator; fs ReadFS }       // equivalent anonymous struct
union { i int; f f64 }                       // anonymous union
&{ mem Allocator; fs ReadFS }
```

The goal is to express one-off structural shapes without introducing named top-level declarations.

This proposal is designed to compose with:

- HEP-12 contexts (`context { ... }`)
- HEP-13 compound literals (inferred `{ field: ... }` targets)

## Motivation

Some APIs need narrow structural contracts that are only used once. Requiring a named declaration
for each shape adds boilerplate and hides the local contract.

## Syntax

```ebnf
Type                    = ... | AnonStructType | AnonUnionType ;
AnonStructType          = ["struct"] "{" [AnonFieldDeclList] "}" ;
AnonUnionType           = "union" "{" [AnonFieldDeclList] "}" ;
AnonFieldDeclList       = AnonFieldDecl { AnonFieldSep AnonFieldDecl } [AnonFieldSep] ;
AnonFieldSep            = ";" ;
AnonFieldDecl           = Ident { "," Ident } Type ;
```

Notes:

- `struct` keyword is optional for anonymous structs.
- `union` keyword is required for anonymous unions.
- Anonymous aggregate types are valid anywhere `Type` is valid.
- In particular, `context` clauses use `Type` (see HEP-12 alignment).

## Examples

Requested forms:

```hop
struct Example {
    pos   { x, y int }                  // anonymous struct type
    size  struct { w int; h int }       // `struct` keyword is optional
    value union { i int; f f64 }        // anonymous union requires `union`
}

fn f_struct(pos { x, y int })
fn f_struct2(pos struct { x, y int })
fn f_union(value union { i int; f f64 })
```

Context + compound-literal interaction:

```hop
fn run() context { mem mut&__hop_Allocator, console u64 } {
    var p { x, y int } = { x: 1, y: 2 }
    _ = p
}
```

Out of scope in HEP-14 (see HEP-16):

```hop
struct Example {
    kind enum { A, B }
}

fn f_enum(kind enum { A, B })

fn main() {
    var x = Example{ kind: .A }
    f_enum(.B)
}
```

## Semantics

### 1. Structural identity

Two anonymous struct types are identical iff they have:

- same field count
- same field names in the same order
- pairwise-equal field types

Two anonymous union types use the same identity rule.

Struct and union kinds are distinct; they are never identical to each other.

### 2. Assignability (general)

Outside context compatibility (below), assignability is exact-match only:

- anonymous struct-to-struct: assignable only when structurally identical
- anonymous union-to-union: assignable only when structurally identical
- no struct<->union implicit conversion
- no width/depth subtyping
- no implicit named<->anonymous aggregate conversion

### 3. Context compatibility (HEP-12 carve-out)

HEP-12 context satisfaction remains structural-by-required-fields and name-sensitive.

That rule applies whether context type is named or anonymous:

- callee context declares required fields
- effective call context must provide each required field by name with assignable type

This is intentionally broader than exact-match assignability.

### 4. Selectors and lookup

Field selection uses normal selector rules (`v.mem`, `v.fs`).

### 5. Compound literal interaction (HEP-13)

`TypeName{ ... }` remains unchanged (explicit named target type form).

For anonymous struct/union targets, use inferred form in typed context:

```hop
var x { a i32; b i32 } = { a: 1, b: 2 }
var y union { i int; f f64 } = { i: 1 }
```

If inferred `{ ... }` is ambiguous, disambiguate with explicit type context, for example:

```hop
var x { a i32; b i32 } = { a: 1, b: 2 }
var y = ({ a: 1, b: 2 } as { a i32; b i32 })
```

### 6. Context overlay interaction (HEP-12)

`with { ... }` semantics are unchanged:

- overlay bind names are validated against caller context fields
- overlay is call-local
- callee requirements are validated after overlay application

### 7. ABI/codegen

Backends may materialize stable synthetic names for anonymous aggregate types.

For C backend, emit deterministic synthetic names derived from canonical shape (kind + field list).

## Diagnostics

New diagnostics expected for HEP-14:

- `anon_aggregate_field_duplicate`: duplicate field `'{s}'`
- `anon_struct_field_missing_type`: field `'{s}'` missing type
- `anon_aggregate_type_mismatch`: anonymous aggregate type mismatch

Existing diagnostics reused:

- `compound_infer_no_context`
- `compound_infer_ambiguous`
- `compound_field_unknown`
- `compound_field_type_mismatch`

## Implementation notes

### Parser

- Add anonymous-aggregate type parsing in type context.
- Update type-start checks to include `{`.
- Parse function `context` clause as `Type` (not only `TypeName`).

### Typechecker

- Add interned anonymous struct/union type kinds with canonical structural keys.
- Extend field lookup to support anonymous aggregate type IDs.
- Keep general assignability exact-match for anonymous aggregates.
- Keep HEP-12 context compatibility as required-field satisfaction.
- Extend compound literal target resolution to accept anonymous aggregate expected types.

### Codegen (C backend)

- Extend type lowering for anonymous aggregate types.
- Emit synthetic C type declarations once per unique anonymous shape.
- Update context argument synthesis to enumerate fields from anonymous context shapes.

## Test plan

1. Positive:
   - function parameter with `&{...}` and `union { ... }`
   - local variable with `{...}` and `struct { ... }`
   - `context { ... }` function and call compatibility
   - inferred compound literal into anonymous typed variable
   - `with { field: { ... } }` where field type is anonymous aggregate
2. Negative:
   - duplicate field names in anonymous aggregate type
   - reordered fields rejected under exact-match assignability
   - ambiguous inferred compound literal with anonymous targets
   - missing required context field with anonymous context
   - struct/union kind mismatch under exact-match rule

## Non-goals

- Anonymous aggregate methods.
- Width/depth subtyping for general assignability.
- Recursive anonymous types in HEP-14.
- Anonymous enums (proposed separately in HEP-16).
