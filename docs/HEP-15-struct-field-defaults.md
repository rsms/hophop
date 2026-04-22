# HEP-15 struct field defaults (completed)

## Summary

HEP-15 adds default initializers for `struct` fields:

```hop
struct LoadFileContext {
    mem    MemAllocator
    tmpmem MemAllocator = mem
    fs     ReadFS
}
```

Defaults reduce literal noise and support context-style APIs.


## Motivation

Many struct values repeat predictable assignments. Defaults remove boilerplate while keeping data
layout explicit.


## Syntax

```ebnf
StructFieldDecl  = FieldDecl [FieldDefault] | EmbeddedFieldDecl [FieldDefault] ;
FieldDefault     = "=" Expr ;
```

Constraints:

- Defaults apply to named and embedded struct fields.


## Semantics

### 1. Initialization behavior

When constructing a struct value:

- explicit field initializers win
- omitted fields with defaults use their default expressions
- remaining omitted fields use zero-value

### 2. Reference to sibling fields

A default expression may reference earlier fields in declaration order.

- Allowed: `b = a`
- Rejected: `a = b` (forward reference)

### 3. Evaluation order

Defaults are evaluated in field declaration order after explicit initializers are applied for
earlier fields.

### 4. Purity and side effects

Defaults are ordinary expressions and may call functions. HEP-15 does not add purity restrictions.

### 5. Embedded fields

An embedded field default is evaluated as the embedded field value. Promoted fields from an
earlier embedded field are visible to later defaults, and explicit promoted-field initializers
override values supplied by the embedded default.


## Diagnostics

- `field_default_forward_ref`: default for `'{s}'` references later field `'{s}'`
- `field_default_type_mismatch`: default expression type mismatch for field `'{s}'`


## Implementation notes

- Parser: allow optional `= Expr` in struct field declarations.
- Typechecker: resolve defaults in declaration order with dependency checks.
- Codegen: emit initialization sequence that matches declared evaluation order.


## Test plan

1. Positive:
   - simple literal omitting defaulted field
   - chained defaults referencing earlier fields
   - embedded field default with promoted-field references
2. Negative:
   - forward reference in default
   - default type mismatch
   - embedded field default type mismatch


## Non-goals

- Defaults for `union` fields.
- Defaults on local `var` declarations.
- Constructor overloading system.
