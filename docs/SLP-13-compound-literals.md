# SLP-13 compound literals (completed)

## Summary

SLP-13 adds compound literals in two forms:

```sl
TypeName{ field: expr, ... }   // explicit target type
{ field: expr, ... }           // target type inferred from context
```

This enables concise construction of structs/unions and supports context-overlay ergonomics in
SLP-12.


## Motivation

Current SL has no compound literal syntax. This makes value construction verbose and blocks
important ergonomics for context and config-heavy APIs.


## Syntax

```ebnf
PrimaryExpr       = ... | CompoundLit ;
CompoundLit       = [TypeName] "{" [FieldInitList] "}" ;
FieldInitList     = FieldInit { "," FieldInit } [","] ;
FieldInit         = Ident ":" Expr ;
```

Notes:

- If `TypeName` is omitted, target type must be inferable from expression context.
- Positional initializers are not included.
- If inference is unavailable or ambiguous, explicit `TypeName` is required.


## Semantics

### 1. Target type resolution

For `TypeName{...}`, `TypeName` resolves directly to a concrete aggregate type (`struct` or
`union`).

For `{...}`, target type is inferred from expected type context. Supported contexts:

- initializer with explicit declared type (`var x T = {...}`)
- assignment to typed lvalue (`x = {...}`)
- call argument position when candidate resolution yields one aggregate parameter type

If no expected type is available, or multiple target aggregate types remain, inference fails.

### 1.1 Interaction with SLP-14 anonymous aggregate types

SLP-14 keeps the explicit form unchanged: `TypeName{...}`.

Anonymous aggregate targets are supported through inferred `{...}` when expected type context is
available, for example:

- `var x { a i32; b i32 } = { a: 1, b: 2 }`
- `var y union { i int; f f64 } = { i: 1 }`
- argument position where parameter type is anonymous aggregate
- `with { field: { ... } }` when `field` has anonymous aggregate type

If inference is ambiguous, disambiguate with explicit type context (typed variable/parameter or
cast), for example `({ a: 1, b: 2 } as { a i32; b i32 })`.

### 2. Field rules

- Every initialized field must exist in target type.
- Duplicate field initializers are invalid.
- Field initializer expression must be assignable to field type.

### 3. Omitted fields

- If field has default initializer (SLP-15), use that default.
- Otherwise, use zero-value initialization.

### 4. Evaluation order

Field initializer expressions evaluate left-to-right.

### 5. Address-of and references

`&T{...}` is valid and yields pointer/reference according to surrounding type rules.

- If expected type is `&T`, `{...}` may materialize a temporary `T` and bind read-only reference.
- If expected type is `mut&T`, `{...}` and `&{...}` are invalid because mutable reference
  arguments require a mutable lvalue.

### 6. Overload ambiguity

If `{...}` can satisfy more than one candidate target aggregate type at a call site, inference is
ambiguous and the call is rejected. Use explicit type context to disambiguate (`TypeName{...}`,
typed variable, or cast).


## Examples

### 1. Explicit typed literal

```sl
struct Point {
    x i32
    y i32
}

var p = Point{ x: 10, y: 20 }
```

### 2. Inferred literal in initialization and assignment

```sl
struct Vec2 {
    x i32
    y i32
}

fn example_assign() {
    var a Vec2 = { x: 10, y: 20 }
    a = { x: 10, y: 30 }
}
```

### 3. Omitted fields (zero/default fill)

```sl
struct Config {
    retries    i32 = 3
    timeout_ms i32
}

var a = Config{ timeout_ms: 500 } // retries from default (SLP-15)
var b = Config{}                   // retries = 3, timeout_ms = 0
```

### 4. Nested literal

```sl
struct Size {
    w i32
    h i32
}

struct Window {
    size  Size
    title str
}

var w = Window{
    size: { w: 800, h: 600 }, // inferred as Size from field type
    title: "sl",
}
```

### 5. Call-site inference and reference behavior

```sl
struct Vec2 { x i32; y i32 }
struct Vec3 { x i32; y i32; z i32 }

fn distance(v Vec2) i32
fn distance_by_ref(v &Vec2) i32
fn round(v mut&Vec2)
fn len(v Vec2) i32
fn len(v Vec3) i32

fn example_calls() {
    distance({ x: 11, y: 42 })
    distance_by_ref({ x: 11, y: 42 }) // ok: temporary binds to &Vec2

    round({ x: 11, y: 42 })           // error: mut&Vec2 requires mutable lvalue
    round(&{ x: 11, y: 42 })          // error: &{...} is read-only reference

    len({ x: 11, y: 42 })             // error: ambiguous target type (Vec2 or Vec3)
}
```

### 6. Context overlay value (SLP-12)

```sl
struct Limits {
    max_files  i32
    timeout_ms i32
}

fn main() context platform.Context {
    var err = load("cfg/app.toml") with {
        limits: { max_files: 64, timeout_ms: 5000 }, // inferred as Limits
    }
    _ = err
}
```


## Diagnostics

- `compound_type_required`: missing/invalid type in compound literal
- `compound_infer_no_context`: cannot infer target type for `{...}`
- `compound_infer_ambiguous`: multiple target aggregate types match `{...}`
- `compound_infer_non_aggregate`: inferred target type is not `struct` or `union`
- `compound_field_unknown`: unknown field `'{s}'`
- `compound_field_duplicate`: duplicate field `'{s}'`
- `compound_field_type_mismatch`: field `'{s}'` type mismatch
- `compound_mut_ref_temporary`: cannot pass literal temporary as `mut&`


## Implementation notes

- Parser: accept both `TypeName{...}` and `{...}` as primary expression forms.
- Typechecker:
  - carry unresolved target type for `{...}` until expected type context is known
  - reject missing/ambiguous inference
  - validate aggregate field initialization rules after target is fixed
  - enforce mutable-reference lvalue requirement for calls
- Call resolution: include `{...}` target inference when selecting candidates.
- Codegen: lower resolved compound literal to temporary value initialization in generated C.


## Test plan

1. Positive:
   - explicit typed struct literal
   - inferred literal in typed initializer and assignment
   - inferred literal at call-site with single target aggregate type
   - readonly reference parameter from inferred literal temporary
   - nested struct literal
   - literal with omitted fields (zero/default fill)
2. Negative:
   - inferred literal with no expected type context
   - inferred literal ambiguous across multiple target aggregate types
   - inferred literal for non-aggregate expected type
   - mutable reference parameter from literal temporary (`{...}` and `&{...}`)
   - unknown field
   - duplicate field
   - type mismatch


## Non-goals

- Context-free `{ ... }` literals without expected type.
- Positional aggregate initializers.
- Constructor methods.
