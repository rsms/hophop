# SLP-31 nested type declarations (completed)

## Summary

SLP-31 introduces named type declarations nested inside aggregate declarations.

```sl
struct Collection {
    union Value {
        i i64
        f f64
    }
    struct Item {
        parent &Collection
        next   ?*Item
        value  Value
    }
    head, tail ?*Item
}
fn main() {
    var _ = Collection.Value{ i: 123 }
}
```

This adds a type-member namespace for named aggregates and aliases while keeping runtime
field/selector behavior unchanged.

## Motivation

Some data models need helper types that are tightly coupled to one outer type and should not be
declared at package top-level.

Today this requires flattening names (`CollectionItem`, `CollectionValue`) and managing naming
conventions manually. Nested type declarations keep these relationships local and explicit.

## Syntax

`struct` and `union` bodies are extended to allow nested type declarations in addition to fields.

```ebnf
StructDecl          = "struct" Ident "{" [ StructMemberList ] "}" .
UnionDecl           = "union" Ident "{" [ UnionMemberList ] "}" .

StructMemberList    = StructMember { FieldSep StructMember } [ FieldSep ] .
UnionMemberList     = UnionMember { FieldSep UnionMember } [ FieldSep ] .

StructMember        = StructFieldDecl | NestedTypeDecl .
UnionMember         = FieldDecl | NestedTypeDecl .

NestedTypeDecl      = StructDecl | UnionDecl | EnumDecl | TypeAliasDecl .
```

Notes:

- Nested declarations do not allow `pub`.
- Functions and constants are not allowed in aggregate bodies.
- Nested declarations reuse the same declaration forms as top-level declarations; context controls
  ownership/scope behavior.
- `TypeName` grammar remains `Ident { "." Ident }`; this proposal extends what those segments can
  resolve to.

## Semantics

### 1. Type-member namespaces

Each named aggregate type (`struct`/`union`) has a type-member namespace.

Nested type declarations become members of that namespace:

- `Collection.Value`
- `Collection.Item`

Member type names are not injected into package scope.

### 2. Name lookup in aggregate bodies

Inside `struct`/`union` bodies:

- the enclosing aggregate name is in scope (`Collection` inside `Collection`)
- directly nested type names are in scope unqualified (`Value`, `Item`)
- sibling nested type names are visible regardless of source order

Inside a nested type body:

- self-name is in scope for recursive references (`next ?*Item`)
- enclosing aggregate names remain available (`parent &Collection`)

### 3. External qualification

Outside the owning aggregate body, nested types are referenced with qualification:

```sl
fn consume(v Collection.Value) {}
var x = Collection.Item{ parent: null, value: Collection.Value{ i: 1 } }
```

### 4. Visibility and exports

Nested type declarations are not independently `pub`.

A nested type is reachable from another package only through an exported root type path. For
example, if `Collection` is `pub`, then `pkg.Collection.Value` is importable by that qualified
path.

### 5. Identity and assignability

Named nested types follow existing named-type identity rules:

- `Collection.Value` and `Other.Value` are distinct types
- no implicit conversion is introduced by nesting

### 6. Runtime selectors and value namespaces

This proposal only adds type-member lookup. Runtime selector behavior is unchanged:

- value selectors still resolve fields/method sugar/enum members as today
- `Type.Member` in type position can now resolve nested types in addition to existing type-path use

### 7. Codegen naming

Backends should emit deterministic lowered names for nested types, e.g.:

- `Collection__Value`
- `Collection__Item`

The exact mangling scheme remains backend-defined but must be stable and collision-safe.

## Diagnostics

Recommended diagnostics:

- `nested_type_name_conflict`: duplicate nested type name in same owner
- `nested_member_conflict`: nested type name conflicts with existing field/member name in same owner
- `nested_type_unresolved`: unknown nested type in qualified path
- `nested_decl_invalid_in_aggregate`: unsupported declaration kind inside aggregate body

## Implementation notes

### Parser

- Extend aggregate member parsing to accept nested type declarations in `struct` and `union` bodies.
- Keep existing field declaration parsing and separator behavior.
- Reject disallowed declaration kinds in aggregate bodies with dedicated diagnostics.

### Typechecker

- Add member-type symbol tables on named aggregate type declarations.
- Perform two-phase aggregate body processing:
  1. register nested type names for cycle-safe lookup
  2. resolve field and nested type bodies
- Extend `TypeName` resolution to traverse nested type-member namespaces.

### Codegen (C backend)

- Emit nested types as top-level C declarations with deterministic mangled names.
- Ensure forward declarations cover recursive references between sibling nested types.

## Test plan

1. Positive:
   - nested `struct` + `union` inside `struct` (example above)
   - recursive nested type reference (`?*Item`)
   - reference to outer type from nested type (`&Collection`)
   - qualified compound literal `Collection.Value{ ... }`
   - cross-file/package use through exported root (`pkg.Collection.Value`)
2. Negative:
   - duplicate nested type name in one owner
   - nested type name conflicting with field name
   - unqualified external use (`Value{...}` outside owner scope) rejected
   - unsupported nested declaration kind (e.g. `fn` inside `struct`) rejected
   - unknown qualified member type (`Collection.Missing`) rejected

## Non-goals

- Nested function/const declarations inside aggregates.
- Instance-associated nested types (no runtime capture of outer value).
- Changes to runtime field selector precedence.
- Local (function-scope) type declarations.

## Future work (separate SLPs)

- Nested `const` declarations inside aggregates (associated constants).
- Nested function declarations inside aggregates.
