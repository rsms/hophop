# SLP-35 lightweight parametric generics

## Summary

SLP-35 adds lightweight parametric generics for the most common cases of reusable types and
functions.

The core surface syntax is:

```sl
struct Vector[T] {
    x, y T
}

fn add[T](a, b Vector[T]) Vector[T] {
    return { x: a.x + b.x, y: a.y + b.y }
}

fn generic_function[T](x, y T) T {
    return x + y
}

fn main() {
    var v1 Vector[i64] = Vector[i64]{ x: -1 as i64, y: 4 }
    var v2 Vector[i64] = Vector[i64]{ x: 5, y: -2 }
    var v3 = add(v1, v2)
    var v4 = v1.add(v2)
    var v5 = generic_function(1 as i8, 2)
}
```

This proposal is intentionally small:

- generic parameters are type-only
- generic functions infer type arguments from ordinary call arguments and, when needed, expected
  result type
- generic named types require explicit type arguments in type and compound-literal positions
- expression-context type values with instantiated or constructed types require an explicit
  `type` prefix
- explicit function type arguments are not part of the language

In particular:

- `add[i64](v1, v2)` is invalid; write `add(v1, v2)`
- `Vector{ x: 1, y: 2 }` does not infer `Vector[T]` in v1; write `Vector[i64]{ ... }`
- `typeof(thing) == Vector[i32]` is invalid; write `typeof(thing) == type Vector[i32]`
- `fn makeThing[T]() T` is invalid; write `fn makeThing(T type) T` for explicit type-directed
  construction

## Motivation

SL already has several generic-adjacent building blocks:

- compile-time type values (`type`, `typeof`, `kind`, `type_name`, `ptr`, `slice`, `array`)
- `anytype`-driven function template instantiation
- template-instance branch specialization for const-evaluable conditions

These are enough for advanced library tricks, but they are not ergonomic for the most common
same-type reusable APIs:

- containers such as `Vector[T]`, `Option[T]`, `Pair[K, V]`
- helpers such as `min[T](a, b T) T` where the generic type of one argument must match another
- receiver-sugar methods over generic types such as `v.add(other)`

The goal is to cover those common cases without introducing:

- a trait/constraint system
- explicit function-instantiation syntax
- runtime boxing or erased generic dispatch
- parser ambiguity in ordinary call syntax

## Goals

- Add a small, explicit generic syntax for named types and functions.
- Reuse the existing compile-time type-value model.
- Keep generic calls readable: `f(x)`, not `f[T](x)`.
- Keep backend lowering simple by requiring full instantiation before MIR/codegen.
- Preserve existing selector-call sugar (`recv.f(args...)`) for generic receiver-first functions.

## Non-goals

SLP-35 does not add:

- trait bounds, interfaces, or constraint clauses
- explicit function type-argument syntax (`f[T](...)`)
- generic inference for named-type compound literals (`Vector{...}`)
- bare expression-level type application syntax (`Vector[i64]` as an ordinary value expression
  without `type` prefix)
- generic local declarations
- runtime reified generic metadata or dynamic generic dispatch

## Syntax

### 1. Type parameter lists on declarations

Named `struct`, `union`, `enum`, `type`, and `fn` declarations may declare type parameters.

```ebnf
TypeParamList    = "[" TypeParam { "," TypeParam } "]" .
TypeParam        = Ident .

StructDecl       = "struct" Ident [ TypeParamList ] "{" [ StructFieldDeclList ] "}" .
UnionDecl        = "union" Ident [ TypeParamList ] "{" [ FieldDeclList ] "}" .
EnumDecl         = "enum" Ident [ TypeParamList ] Type "{" [ EnumItemList ] "}" .
TypeAliasDecl    = "type" Ident [ TypeParamList ] Type .
FnDeclOrDef      = "fn" FnName [ TypeParamList ] "(" [ ParamList ] ")" [ FnResultClause ]
                   [ ContextClause ] [ Block ] .
```

Examples:

```sl
struct Pair[A, B] {
    first  A
    second B
}

type Ptr[T] *T

fn first[T](x T, _ T) T {
    return x
}
```

### 2. Type argument lists in type positions

Instantiated generic named types are written with type arguments in type positions:

```ebnf
TypeName         = Ident { "." Ident } [ TypeArgList ] .
TypeArgList      = "[" Type { "," Type } "]" .
```

Examples:

```sl
var p Pair[i32, &str]
fn read(v Vector[i64]) i64
type Bytes = Vector[u8]
```

### 3. Compound literals for instantiated generic named types

The type prefix of a compound literal may use an instantiated generic named type:

```sl
var v = Vector[i64]{ x: 1, y: 2 }
```

In v1, bare `Vector{ ... }` is invalid when `Vector` is generic.

### 4. No explicit function type arguments

There is no expression syntax for explicitly instantiating a generic function.

`f[T](x)` is not a generic-call form.

That source continues to parse as ordinary postfix indexing plus call. If `f` names a function,
the expression is invalid because functions are not indexable.

Generic function calls must be written as:

```sl
f(x)
recv.f(x)
pkg.f(x)
```

### 5. `type`-prefixed type values in expression context

This proposal adds an explicit expression form for type values:

```ebnf
TypeValueExpr    = "type" Type .
```

This form is used when a type expression appears in ordinary expression context and the spelling
would otherwise collide with existing expression grammar.

Examples:

```sl
if typeof(thing) == type Vector[i32] {}
if typeof(thing) == type &[i32] {}
const P = type *i64
```

Rules:

- `type Vector[i32]` is the required spelling for an instantiated generic named type used as a
  value expression
- `type &[i32]`, `type *T`, `type fn(i32) i32`, and similar constructed-type values are also
  allowed
- existing simple type-name value expressions such as `i64` remain unchanged
- the `type` prefix is only for expression-context type values; it is not used in ordinary type
  positions

## Semantics

### 1. Type parameters are compile-time type variables

Each type parameter denotes a compile-time value of builtin metatype `type`.

Inside a generic declaration body:

- the parameter may be used in type positions (`*T`, `Vector[T]`, `fn(T) T`)
- the parameter may participate in compile-time reflection (`T == i64`, `kind(T)`, `type_name(T)`,
  `typeof(v) == type Vector[T]`)

This aligns generic parameters with the existing type-value model instead of introducing a separate
kind of meta-variable.

### 2. Generic named types

A generic named type declaration defines a family of named concrete instantiations.

Examples:

- `Vector[i64]`
- `Vector[int]`
- `Pair[i32, &str]`

Rules:

- type-argument count must match declaration arity exactly
- each type argument must be a well-formed SL type
- different argument vectors produce distinct named types
- fields, defaults, embedded-base rules, and member lookup are checked after substitution

Examples:

- `Vector[i64]` and `Vector[int]` are distinct types
- `Pair[i32, &str]` and `Pair[i32, str]` are distinct types

### 3. Generic functions

A generic function declaration defines a template root plus zero or more concrete instances.

Example:

```sl
fn add[T](a, b Vector[T]) Vector[T] {
    return { x: a.x + b.x, y: a.y + b.y }
}
```

At each call site, the compiler infers a concrete type argument vector and typechecks the call
against the corresponding instantiated signature.

Implementation strategy is monomorphization or equivalent lowering that preserves the same static
semantics.

### 4. Function type-argument inference

Inference for a generic function call proceeds in this order:

1. infer from ordinary call arguments by matching parameter types against argument types
2. if some parameters remain unresolved and the call has an expected result type, infer from the
   instantiated return type against that expected result
3. as a last resort, if an unresolved type parameter is constrained only by untyped literal
   arguments, default those literals using the same concretization rules as `var` declaration
   inference
4. if any type parameter remains unresolved, the call is invalid

The last-resort defaulting step reuses the existing variable-inference defaults:

- `const_int -> int`
- `const_float -> f64`

Examples:

```sl
fn id[T](x T) T
var a i64 = id(1 as i64)     // T = i64 from argument
var _ = id(1)                // T = int by untyped-literal fallback

fn make[T](self Factory) T
var b i64 = f.make()         // T = i64 from expected result
var c = f.make()             // invalid: cannot infer T
```

### 5. Repeated type parameters mean exact same type

If the same type parameter appears multiple times, all occurrences refer to the same concrete type.

```sl
fn same[T](a, b T) T
```

For `same(1 as i8, 2)`, inference fixes `T = i8` from the first argument, then checks that the
second argument is assignable to `i8`.

### 6. Zero-argument generic-return functions are invalid

A generic function is invalid if:

- it has zero ordinary parameters, and
- one or more of its type parameters appear in the return type

Example:

```sl
fn makeThing[T]() T // invalid
```

Rationale:

- explicit function type arguments are not supported
- this form behaves like a hidden type-constructor call surface
- SL already has an explicit and simpler spelling for this use case: ordinary functions with
  `type` parameters

Write this instead:

```sl
fn makeThing(T type) T
```

This restriction is only about the zero-argument case.

These remain valid declarations:

```sl
fn cast_like[T](x i64) T          // may infer T from expected result
fn makeThing[T](self Factory) T   // may infer T from expected result at call site
fn zero[T](x T) T                 // T inferred from argument
```

Calls that still do not provide enough information remain errors.

### 7. Generic receiver-first functions and selector sugar

Existing selector-call sugar continues to apply after inference:

```sl
fn add[T](self Vector[T], other Vector[T]) Vector[T]

v.add(other)
```

This resolves exactly like:

```sl
add(v, other)
```

with the same inference and overload-resolution rules.

### 8. Generic functions with `type` parameters remain supported

This proposal does not replace ordinary functions that explicitly take `type` values.

Those remain the correct spelling when the caller must choose a type explicitly or when there is no
usable inference path:

```sl
fn makeThing(T type) T
fn alloc_ptr(T type) *T
```

SLP-35 generics are for inferred parametric reuse. Explicit `type` parameters remain the
escape hatch for APIs that are naturally type-directed rather than argument-directed.

### 9. Interaction with compile-time evaluation

Generic function instances behave like specialized ordinary functions for compile-time checking.

Consequences:

- generic bodies may branch on `T` or on types derived from generic arguments
- const-evaluable conditions in instantiated generic bodies may specialize branch checking
- reflection on type parameters works through the existing `type`-value operations

Example:

```sl
fn score[T](x T) i64 {
    if T == i64 {
        return (x + 1 as i64) - 1 as i64
    } else {
        return len(x) as i64
    }
}
```

### 10. Lowering requirement

All generic named-type and generic-function uses must be fully instantiated before backend-facing
MIR/codegen.

Backends do not perform late generic resolution.

## Type identity and assignability

### 1. Generic named types

- `Vector[i64]` is assignable only where that exact instantiated named type is required, subject to
  ordinary existing assignability rules
- `Vector[i64]` is not implicitly convertible to `Vector[int]`

### 2. Generic functions

Generic functions are not a new overload dimension beyond their instantiated signatures.

Operationally:

- a generic function root participates in call resolution by attempting inference
- failure to infer removes that candidate
- successful inference produces an instantiated candidate
- ordinary conversion-cost ranking and ambiguity rules then apply

## Restrictions

### 1. Explicit type arguments are only for named types

`Foo[T]` syntax is only valid in named-type declaration headers and named-type use sites.

It is not an expression-level generic instantiation form for functions.

When a generic instantiated type is used as a value expression, write `type Foo[T]`.

### 2. No generic named-type literal inference in v1

For generic named types, the compound-literal type prefix must be explicit:

```sl
Vector[i64]{ x: 1, y: 2 } // ok
Vector{ x: 1, y: 2 }      // invalid in v1
```

This keeps parsing and type inference simple and avoids introducing a second inference surface
separate from function-call inference.

### 3. No unconstrained type parameters

If inference cannot determine every type parameter of a called generic function, the call is an
error.

This includes cases where the function declaration itself is valid but the call has no argument or
expected-result information sufficient to choose concrete types.

## Diagnostics

Recommended diagnostics:

- `generic_type_arity_mismatch`: wrong number of type arguments for generic type
- `generic_type_args_required`: generic named type requires explicit type arguments here
- `generic_fn_type_args_forbidden`: explicit function type arguments are not supported
- `generic_fn_cannot_infer`: cannot infer generic type argument `'{s}'`
- `generic_fn_zero_arg_return_forbidden`: generic function with zero ordinary parameters cannot
  use type parameters in return position
- `generic_param_unused`: type parameter is declared but unused

## Compatibility and migration

Backward-compatible: existing non-generic programs are unchanged.

This proposal coexists with:

- explicit `type`-parameter functions
- `anytype`
- receiver sugar
- compile-time reflection

Migration guidance:

- use SLP-35 generics for same-type reusable APIs
- keep `fn f(T type) ...` for explicitly type-directed construction

## Implementation notes

### Parser

- add `TypeParamList` after declaration names for named type declarations and functions
- add `TypeArgList` in named-type use positions
- keep expression grammar unchanged for calls; do not add function-instantiation syntax
- reject or diagnose `f[T](...)` as non-function generic application

### Typechecker

- represent generic type parameters as compile-time type variables
- instantiate generic named types by substituted concrete type vectors
- instantiate generic functions by inferred concrete type vectors
- extend call inference to use expected result type as a secondary inference source
- enforce the zero-argument generic-return restriction at declaration checking time

### Lowering/codegen

- generic named types and generic functions lower as concrete instantiated entities
- reuse the existing template-instance machinery where possible
- ensure no generic placeholders survive backend-facing MIR

## Test plan

1. Positive:
   - generic `struct Vector[T]`
   - generic function `fn add[T](a, b Vector[T]) Vector[T]`
   - selector sugar over generic receiver-first function
   - inference from arguments (`generic_function(1 as i8, 2)`)
   - inference from expected result for non-zero-arg generic-return function
2. Negative:
   - `add[i64](v1, v2)` rejected
   - `Vector{ ... }` rejected when `Vector` is generic
   - generic type arity mismatch
   - `fn makeThing[T]() T` rejected
   - call with unresolved type parameter rejected
   - mismatched same-`T` arguments rejected

## Future work

- generic compound-literal type inference for named types
- explicit function-instantiation syntax, if a concrete use case appears later
- generic constraints/traits
- type-value ergonomics without mandatory `type` prefix (`const V = Vector[i64]`)
