# SLP-14 anonymous struct types (draft)

## Summary

SLP-14 adds inline anonymous struct type syntax:

```sl
{ mem MemAllocator, fs ReadFS }
&{ mem MemAllocator, fs ReadFS }
```

This allows function signatures and local declarations to express narrow structural shapes without
introducing named top-level types.


## Motivation

Some APIs need a one-off record shape. Requiring a named top-level `struct` for every such shape
adds boilerplate and hides intent at the call site.

SLP-12 context clauses also benefit from inline shapes.


## Syntax

```ebnf
Type            = ... | AnonStructType ;
AnonStructType  = "{" [AnonFieldList] "}" ;
AnonFieldList   = AnonField { "," AnonField } [","] ;
AnonField       = Ident Type ;
```

Examples:

```sl
fn save(path str, opts &{ mem MemAllocator, fs WriteFS }) ?error
var x { a i32, b str }
```


## Semantics

### 1. Structural identity

Two anonymous struct types are identical iff they have:

- same field count
- same field names in the same order
- pairwise-equal field types

### 2. Assignability

Initial rule in SLP-14: exact structural match only.

No width subtyping in SLP-14.

### 3. Name lookup and selectors

Field selection uses normal selector rules (`v.mem`, `v.fs`).

### 4. ABI/codegen

Backends may materialize compiler-internal generated names for anonymous types.


## Diagnostics

- `anon_struct_field_duplicate`: duplicate field `'{s}'`
- `anon_struct_field_missing_type`: field `'{s}'` missing type
- `anon_struct_type_mismatch`: anonymous struct type mismatch


## Implementation notes

- Parser: add anonymous field-list parsing in type context.
- Typechecker: intern canonical structural type IDs.
- Codegen: emit stable synthetic names for generated C declarations.


## Test plan

1. Positive:
   - function parameter with `&{...}`
   - local variable with `{...}`
   - assignment between identical shapes
2. Negative:
   - duplicate field names
   - reordered fields rejected under exact-match rule


## Non-goals

- Anonymous struct methods.
- Width/depth subtyping.
- Recursive anonymous types in SLP-14.
