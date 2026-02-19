# SLP-8 parameter type runs

## Summary

SLP-8 adds compressed parameter type runs in function parameter lists.

Examples:

```sl
fn resize(oldSize, newSize, align, size uint) uint
fn rotate(v mut&Vec3, x, y, z f32)
fn example3(a, b int, c uint)
```

Existing explicit syntax remains valid:

```sl
fn resize(oldSize uint, newSize uint, align uint, size uint) uint
```

---

## Motivation

Current syntax requires repeating the same type for each parameter. With many parameters sharing a
type, signatures become noisy and harder to scan.

Compressed runs reduce repetition while keeping type information explicit at the end of each run.

---

## Syntax

Replace function parameter grammar with:

```ebnf
FnDeclOrDef     = "fn" Ident "(" [ParamList] ")" [Type] (";" | Block) ;
ParamList       = ParamGroup { "," ParamGroup } ;
ParamGroup      = Ident { "," Ident } Type ;
```

Equivalent expansion:

- `a, b, c T` is equivalent to `a T, b T, c T`.

Notes:

- This applies only to function parameter lists.
- Function-group syntax (`fn name{...};`) is unchanged.

---

## Semantic rules

### 1. Canonical meaning

Each `ParamGroup` desugars to one parameter per name, all with the same type.

Examples:

- `fn f(a, b int)` => `fn f(a int, b int)`
- `fn f(a, b int, c uint)` => `fn f(a int, b int, c uint)`

### 2. Type checking and calls

After desugaring, parameter typing, call arity checks, and assignability behavior are unchanged.

### 3. Missing trailing type in group

A parameter name that is not followed by a type in its group is invalid.

Example:

```sl
fn example4(a, b int, c) // error: parameter 'c' is missing type
```

---

## Diagnostics

Recommended diagnostic:

- `param_missing_type`: parameter `'{s}'` is missing type

`EXPECTED_TYPE` may still be used as parser fallback, but SLP-8 recommends a dedicated diagnostic
for clearer user feedback.

---

## Compatibility and migration

- Feature is additive.
- Existing function signatures remain valid.
- No source migration is required.

---

## Implementation notes

### Parser

Current parser reads one parameter as `Ident Type`. SLP-8 should parse grouped names before type:

1. Read one or more names separated by `,`.
2. Require one trailing type for that group.
3. Emit one `SLAst_PARAM` node per name.

Important:

- Emit an explicit type node for each emitted `SLAst_PARAM` (no shared child node).
- Keep emitted AST in canonical expanded form so downstream phases do not need SLP-8-specific logic.

### Type checker and codegen

No functional changes expected if parser emits canonical expanded parameter nodes.

---

## Test plan

Add parser/check tests for:

1. Positive:
   - `fn resize(oldSize, newSize, align, size uint) uint`
   - `fn rotate(v mut&Vec3, x, y, z f32)`
   - `fn f(a, b int, c uint)`
2. Negative:
   - `fn f(a, b int, c)` -> missing type diagnostic on `c`
3. Regression:
   - existing explicit `name Type` parameters continue to work
4. AST:
   - snapshot proving canonical expanded parameter nodes

---

## Non-goals

SLP-8 does not add:

- compressed runs for locals, fields, or other declaration kinds
- changes to return type syntax
- changes to function-group declarations
- parameter-list trailing-comma syntax changes

