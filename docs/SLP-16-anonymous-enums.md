# SLP-16 anonymous enums with contextual identification (draft)

## Summary

SLP-16 proposes anonymous enum types and contextual enum member literals:

```sl
enum { A, B }
.A
```

Anonymous enums are written inline in type position. Enum member literals are written as `.Name`
and resolved using expected enum type context.

## Motivation

Anonymous aggregate types in SLP-14 cover one-off struct/union shapes. Some APIs also need small
local choice sets without introducing a named top-level enum.

To keep call-sites concise, enum values should be writable as `.A` when type context is already
known.

## Syntax

```ebnf
Type                    = ... | AnonEnumType ;
AnonEnumType            = "enum" "{" [AnonEnumItemList] "}" ;
AnonEnumItemList        = Ident { EnumItemSep Ident } [EnumItemSep] ;
EnumItemSep             = "," | ";" ;

PrimaryExpr             = ... | ContextualEnumItemExpr ;
ContextualEnumItemExpr  = "." Ident ;
```

Notes:

- `enum` keyword is required for anonymous enums.
- `.Name` is only valid where enum expected type can be determined.

## Semantics

### 1. Type identity

Two anonymous enum types are identical iff they have:

- same item count
- same item names in the same order

### 2. Assignability

- anonymous-enum to anonymous-enum is allowed only for identical item lists
- no implicit conversion between anonymous enums and named enums
- no numeric implicit conversion for enum values

### 3. Contextual enum item resolution

`.Item` is resolved against expected enum type context.

Supported expected-type contexts:

- typed variable initialization (`var k enum { A, B } = .A`)
- assignment to typed lvalue
- call argument position after candidate filtering determines parameter enum type
- compound literal field initialization when field type is known

Resolution rules:

1. expected type must be an enum type
2. expected enum must contain item name
3. result type is the expected enum type

If expected type is unavailable or ambiguous, `.Item` is rejected.

### 4. Overload interaction

If `.Item` appears in a call and multiple overload candidates remain with different enum expected
types that all contain `Item`, the call is ambiguous.

### 5. Example

```sl
struct Example {
    kind enum { A, B }
}

fn f_enum(kind enum { A, B })

fn main() {
    var x = Example{ kind: .A }
    f_enum(.B)
}
```

## Diagnostics

Proposed diagnostics:

- `anon_enum_item_duplicate`: duplicate anonymous enum item `'{s}'`
- `contextual_enum_item_no_context`: cannot resolve enum item literal without expected enum type
- `contextual_enum_item_unknown`: enum item `'{s}'` not found in expected enum type
- `contextual_enum_item_ambiguous`: enum item literal resolution is ambiguous

## Implementation notes

### Parser

- Parse `enum { ... }` in type position.
- Parse `.Ident` as contextual enum item expression in expression position.

### Typechecker

- Intern anonymous enum types by canonical item-list key.
- Extend expected-type-driven expression typing to resolve `.Ident`.
- Integrate `.Ident` into call candidate filtering and ambiguity diagnostics.

### Codegen (C backend)

- Emit stable synthetic enum type names for anonymous enums.
- Emit enumerators with deterministic synthetic prefixes.
- Lower contextual literals to resolved enum constants after typecheck.

## Test plan

1. Positive:
   - typed initialization with `.A`
   - call argument `.B` resolved by parameter type
   - compound literal field init using `.A`
2. Negative:
   - `.A` without expected type context
   - unknown item in expected enum
   - duplicate enum items
   - ambiguous call-site contextual enum literal

## Non-goals

- Anonymous enum methods.
- Flag enums/bitset semantics.
- Implicit conversions between enum and integer types.
