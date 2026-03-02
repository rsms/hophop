# SLP-24 const function parameters (draft)

## Summary

SLP-24 adds a `const` modifier for function parameters to mean:

- argument expressions bound to that parameter must be const-evaluable at the call site
- Inside the function, a `const` parameter is treated as a compile-time constant. You can use it in places that require compile-time-known values like with consteval.

Example:

```sl
fn parse_base(const base u32, text &str) int {
    // `base` is guaranteed const-evaluable at each call site.
    return 0
}
```

This is a call-site constraint. It does not change pointer/reference mutability semantics.

## Motivation

Some APIs need compile-time-known arguments for validation or specialization while still running as
ordinary runtime functions.

Current SL has const-eval behavior, but there is no direct way to declare this requirement in a
function signature.

`const` parameters provide an explicit, local contract.

## Syntax

### Function declarations

`ParamGroup` becomes:

```ebnf
ParamGroup = "const" (( Ident | "_" ) Type | ( Ident | "_" ) "..." Type)
           | (( Ident | "_" ) { "," ( Ident | "_" ) } Type
           | ( Ident | "_" ) "..." Type) .
```

Examples:

```sl
fn f(const n u32)
fn g(const a i32, b i32)
fn h(const args ...i32)
```

### Function types

`FnTypeParam` becomes:

```ebnf
FnTypeParam = "const" ( Type | Ident Type | "..." Type )
            | Type
            | ( Ident { "," Ident } Type )
            | "..." Type .
```

Examples:

```sl
type P = fn(const u32, &str) int
type Q = fn(const n u32, x &str) int
```

## Semantic rules

### 1. Call-site const-evaluable requirement

For each argument bound to a `const` parameter, the argument expression must be const-evaluable
under Reference-slc const-eval rules.

If binding uses named arguments, the same rule applies after name mapping.

For variadic `const` parameters, the rule applies to every bound variadic element.

### 2. No new mutability meaning

`const` on parameters does not mean read-only storage or immutable reference semantics.
Those remain controlled by the existing type (`*T`, `&T`, etc).

### 3. Signature and type identity

`const`-ness is part of function signature identity and function type identity.

- `fn f(x i32)` and `fn f(const x i32)` are different signatures.
- `fn(i32)` and `fn(const i32)` are distinct function types.

### 4. Overload resolution

Candidate filtering includes `const` parameter requirements.
A candidate is non-viable if any bound `const` parameter argument is not const-evaluable.

## Diagnostics

Recommended new diagnostics:

- `const_param_arg_not_const`: argument is not const-evaluable for `const` parameter `'{s}'`
- `const_param_spread_not_const`: spread value includes non-const-evaluable element(s) for `const`
  variadic parameter `'{s}'`

## Compatibility and migration

This change is backward-compatible for existing code.

`const` is already a keyword; this proposal only adds a new valid parse position in parameter lists
and function type parameter lists.

## Implementation notes

Parser:

- accept `const` in parameter positions
- reject grouped-name const forms (`const a, b T`) and emit a targeted diagnostic with rewrite hint

Typechecker:

- track `const` marker per parameter in function signatures
- during call binding, evaluate const-evaluable predicate for arguments bound to `const` params
- include `const` marker in function-type equivalence and overload identity checks

Codegen:

- no direct ABI change required; this is a static-checking constraint

## Test plan

Add tests for:

1. Positive:
   - const literal argument
   - const expression argument
   - named argument bound to const parameter
2. Negative:
   - non-const local variable argument
   - non-const function call argument
   - variadic const parameter with at least one non-const element
   - grouped-name const parameter form rejected (`const a, b T`)
3. Function-type identity:
   - assignment mismatch between `fn(T)` and `fn(const T)`

## Non-goals

SLP-24 does not add:

- local-variable `const` modifiers beyond existing declaration forms
- field-level or type-qualifier const systems
- runtime immutability enforcement

## Resolved decisions

1. `const` parameters are restricted to single-name forms for clarity.
   - `const a, b T` is invalid.
   - rewrite as `const a T, b T`.
2. Diagnostics include the parameter name by default.
   - example shape: `const parameter 'format' requires const-evaluable argument`.
