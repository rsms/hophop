# SLP-13 compound literals (draft)

## Summary

SLP-13 adds typed compound literals:

```sl
TypeName{ field = expr, ... }
```

This enables concise construction of structs/unions and supports context-overlay ergonomics in
SLP-12.

---

## Motivation

Current SL has no compound literal syntax. This makes value construction verbose and blocks
important ergonomics for context and config-heavy APIs.

---

## Syntax

```ebnf
PrimaryExpr       = ... | CompoundLit ;
CompoundLit       = TypeName "{" [FieldInitList] "}" ;
FieldInitList     = FieldInit { "," FieldInit } [","] ;
FieldInit         = Ident "=" Expr ;
```

Notes:

- Type is required in SLP-13 (`T{...}` only).
- Positional initializers are not included.

---

## Semantics

### 1. Type resolution

`TypeName{...}` resolves `TypeName` to a concrete aggregate type (`struct` or `union`).

### 2. Field rules

- Every initialized field must exist in target type.
- Duplicate field initializers are invalid.
- Field initializer expression must be assignable to field type.

### 3. Omitted fields

- If field has default initializer (SLP-15), use that default.
- Otherwise, use zero-value initialization.

### 4. Evaluation order

Field initializer expressions evaluate left-to-right.

### 5. Address-of

`&T{...}` is valid and yields pointer/reference according to surrounding type rules.

---

## Diagnostics

- `compound_type_required`: missing/invalid type in compound literal
- `compound_field_unknown`: unknown field `'{s}'`
- `compound_field_duplicate`: duplicate field `'{s}'`
- `compound_field_type_mismatch`: field `'{s}'` type mismatch

---

## Implementation notes

- Parser: add `TypeName "{" ... "}"` as primary expression form.
- Typechecker: validate target aggregate and field initializers.
- Codegen: lower to temporary value initialization in generated C.

---

## Test plan

1. Positive:
   - simple struct literal
   - nested struct literal
   - literal with omitted fields (zero/default fill)
2. Negative:
   - unknown field
   - duplicate field
   - type mismatch

---

## Non-goals

- Anonymous/typeless literals (`{ ... }`) without contextual type.
- Positional aggregate initializers.
- Constructor methods.
