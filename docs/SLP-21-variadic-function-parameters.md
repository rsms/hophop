# SLP-21 variadic function parameters (draft)

## Summary

SLP-21 adds Go-style variadic function parameters.

A variadic parameter is the last parameter in a function signature and is written as `name ...T`.

```sl
fn sum(nums ...int) int
fn printf(format &str, args ...Value)
```

Callers can:

- pass zero or more extra positional arguments of type `T`
- pass one existing slice value using `...`

```sl
sum()
sum(1, 2, 3)

var xs [int] = some_ints()
sum(xs...)
```

This proposal is intended as a backend-agnostic language feature and a prerequisite for pure-SL
formatting and logging APIs.

## Motivation

SL currently requires fixed arity at each function declaration. That makes APIs like formatting,
structured logging, and forwarding wrappers awkward.

Go-style variadics are a small, familiar model that:

- keeps static typing (`...T`, not untyped varargs)
- keeps call syntax concise
- avoids backend-specific intrinsics for common library APIs

## Goals

- Add variadic parameters with Go-like rules and call behavior.
- Keep static typechecking deterministic and simple.
- Make the feature available uniformly to all backends.

## Non-goals

- C ABI varargs (`...` with default promotions).
- Implicit dynamic boxing as part of this SLP.
- Changes to overload ranking beyond arity/assignability updates needed for variadics.

## Syntax

### 1. Function parameters

Only the final parameter may be variadic.

```ebnf
FnDeclOrDef     = "fn" Ident "(" [ParamList] ")" [Type] (";" | Block) ;
ParamList       = ParamGroup { "," ParamGroup } ;
ParamGroup      = Ident { "," Ident } Type
                | Ident "..." Type ;
```

Rules:

- variadic form may appear at most once
- variadic form must be the final `ParamGroup`
- variadic form allows exactly one name (no grouped names)

Valid:

```sl
fn f(a int, b ...int)
```

Invalid:

```sl
fn f(a ...int, b int)
fn f(a ...int, b ...int)
fn f(a, b ...int)
```

### 2. Call arguments

Call arguments gain an optional trailing spread marker on the final argument.

```ebnf
CallArg         = [Ident ":"] Expr ["..."] ;
```

Spread rules:

- `...` is allowed only on the final call argument
- `...` is valid only when calling a variadic function

## Core semantics

### 1. Parameter meaning

For `fn f(x A, ys ...T)`, inside `f` the binding `ys` is a slice of `T` with Go-like behavior.

This SLP treats `ys` as a normal slice value for operations (`len`, indexing, iteration rules), even
if SL internally uses a dedicated representation.

### 2. Call with explicit variadic elements

A call like `f(a, e1, e2, e3)` (where `f` is variadic) maps `e1..e3` into the variadic slice in
source order.

Type rule: each `ei` must be assignable to `T`.

### 3. Call with spread argument

A call like `f(a, s...)` passes one existing slice `s` as the variadic slice.

Type rule: `s` must be assignable to slice-of-`T`.

As in Go, when `s...` is used, it provides the variadic tail for the call.

### 4. Zero variadic arguments

Calling `f(a)` produces an empty variadic slice (`len == 0`).

### 5. Evaluation order

Argument evaluation order remains left-to-right.

### 6. Function values and type identity

A variadic function type is distinct from a non-variadic function type with a trailing slice
parameter.

Example:

- `fn(...int)` is not identical to `fn([int])` (or its internal equivalent)

## Named arguments interaction

If named arguments are used, normal named-argument rules still apply to fixed parameters.
Variadic tail arguments are positional.

## Diagnostics (draft)

- `variadic_param_not_last`: variadic parameter must be last
- `variadic_param_duplicate`: only one variadic parameter is allowed
- `variadic_arg_type_mismatch`: variadic argument is not assignable to element type
- `variadic_spread_non_slice`: spread argument must be a slice of the variadic element type
- `variadic_spread_not_last`: spread argument must be the final argument
- `variadic_call_shape_mismatch`: invalid argument shape for variadic call

## Lowering model

Backends should lower variadic calls using ordinary slice machinery.

Conceptual lowering:

1. explicit variadic elements: materialize a temporary contiguous backing store and slice descriptor
2. spread form: forward existing slice descriptor
3. zero-tail form: pass empty slice descriptor

No backend-specific language semantics are introduced.

## Test plan

1. Parsing and declaration validity:
- accepted final variadic parameter
- rejected non-final/multiple variadics

2. Call typing:
- zero variadic args
- multiple explicit variadic args
- spread with compatible slice
- spread/type mismatch failures

3. Runtime behavior:
- len/order preservation for explicit args and spread

4. Function values:
- distinct type identity for variadic signatures

## Open questions

1. Should array values be spreadable directly, or only slices (`s...`) as in Go?
    - Answer: array values should be spreadable directly. Any value that is convertible to a slice.
    I.e. `var array [i32 4]; var slice &[i32] = array` is valid, implicitly creating a slice from an array value.
2. Should variadic slice be mutable or read-only by default in SL?
    - Answer: read-only. I.e. the only two operations are `len` and subscript-load `v = args[N]`. E.g. `fn sum(nums ...int) int { len(nums); var num0 = nums[0] }`
3. Should forwarding from one variadic function to another require explicit `...` always?
    - Answer: Yes. Inside a function body with variadic parameters `args`, `args` is conceptually a `&[T]` slice.
