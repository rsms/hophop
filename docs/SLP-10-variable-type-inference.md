# SLP-10 variable type inference

## Summary

SLP-10 allows omitting explicit types in `var` and `const` declarations when an initializer is
present.

Examples:

```sl
fn example(a i32, b str) {
    var c = a       // inferred as i32
    const d = b     // inferred as str
}
```

Existing explicit declarations remain valid:

```sl
var x i32 = 1
const y str = "ok"
var z i32
```

---

## Motivation

Many local declarations repeat obvious type information from the initializer:

```sl
var c i32 = a
const d str = b
```

Allowing inference in these cases improves readability and reduces noise without changing the type
system.

---

## Syntax

Replace declaration forms with:

```ebnf
VarDeclStmt  = "var" Ident ( Type [ "=" Expr ] | "=" Expr ) ";" ;
ConstDecl    = "const" Ident ( Type [ "=" Expr ] | "=" Expr ) ";" ;
```

Notes:

- Type omission is only valid in the `= Expr` form.
- `var x;` and `const x;` remain invalid.
- `var x T;` remains valid and unchanged.

---

## Semantic rules

### 1. Inference source

When type is omitted, declared type is inferred from initializer expression type.

Examples:

- `var c = a` where `a: i32` => `c: i32`
- `const d = b` where `b: str` => `d: str`

### 2. Untyped literal defaulting in inference

When initializer type is untyped, inference applies default concrete types:

- `untyped_int` defaults to `int`
- `untyped_float` defaults to `f64`

Examples:

- `var x = 1` => `x: int`
- `const pi = 3.14` => `pi: f64`

### 3. Invalid inference sources

Type inference is rejected when initializer type is not inferrable:

- `null` literal alone (`var x = null`)
- `void` expression result

These require explicit type annotation.

### 4. Explicit type + initializer (unchanged)

If explicit type is present with initializer, existing assignability checks remain unchanged.

### 5. Initialization requirements (unchanged except inferred form)

- `var x T` is valid (default initialization rules unchanged).
- `const` behavior for missing initializer is unchanged when explicit type is present.
- In inferred form (`= Expr`), initializer is mandatory by syntax.

### 6. Cross-package and transitive named types

Inference uses the resolved type of the initializer expression, including named types that come
from dependencies.

If a function call result type resolves to a named type from another package, inferred declaration
type is that named type, even when the current file does not directly import that defining package.

Example intent:

- Package `A` exports type `X`
- Package `B` imports `A` and exports `fn make() A.X`
- Package `C` imports only `B` and does:
  - `var result = b.make()`
- `result` is inferred as `A.X` (the resolved named type), with no extra import requirement in
  `C`.

This SLP does not add new public-API visibility rules; it only specifies inference behavior when
such signatures are present and valid in the program.

---

## Diagnostics

Recommended diagnostics:

- `infer_null_type_unknown`: cannot infer type from `null`; add explicit type
- `infer_void_type_unknown`: cannot infer type from `void` expression

Parser may still use existing `EXPECTED_TYPE` / `EXPECTED_EXPR` diagnostics for malformed
declarations.

---

## Compatibility and migration

- Feature is additive.
- Existing typed declarations remain valid.
- No source migration is required.

---

## Implementation notes

### Parser

- Update `var` and `const` parsing to accept either:
  - `Ident Type [= Expr]`
  - `Ident = Expr`
- AST may continue to use existing declaration node kinds.
- For inferred declarations, omit type child node and keep initializer child node.

### Type checker

- In declaration typing:
  - If explicit type exists: keep current path.
  - If type omitted: type initializer first, then materialize inferred type.
- Add helper to concretize untyped literals (`untyped_int` -> `int`, `untyped_float` -> `f64`).
- Reject inference from `null` and `void`.
- For named types, keep inferred type as canonical resolved type identity (type id), not by
  re-parsing source spelling. This ensures transitive-package named types infer correctly without
  requiring new imports.

### Codegen

- No new lowering model expected if typechecker writes resolved declaration type into symbol/local
  tables as today.

---

## Test plan

Add tests for:

1. Positive:
   - `var c = a`, `const d = b`
   - `var x = 1` defaults to `int`
   - `const pi = 3.14` defaults to `f64`
   - mixed explicit and inferred declarations in same scope
2. Negative:
   - `var x = null` (cannot infer)
   - `var x = some_void_call()` (no result from function call)
3. Regression:
   - existing explicit declarations still pass

---

## Non-goals

SLP-10 does not add:

- multi-variable declaration syntax (`var a, b = ...`)
- bidirectional inference across declarations
- inference for function parameters, struct/union fields, or enum item types
- generic type inference (language still has no generics)

---

## Decisions (ready to implement)

1. Scope:
   Inferred declarations are allowed anywhere existing declaration forms are allowed.
   In particular, inferred top-level `const` is allowed:
   - `const X = 1`
2. Default integer type:
   `untyped_int` defaults to `int` for inference.
3. Error style:
   Use dedicated diagnostics for failed inference from `null` and `void`.
4. Transitive package types:
   Inference must preserve resolved named types across package boundaries and must not require
   direct import of the defining package in the declaring file.
