# SLP-29 `for ... in` loops (completed)

## Summary

SLP-29 adds a new `for ... in` loop construct with two head forms:

```sl
for VALUE_NAME in EXPR { ... }
for KEY_NAME, VALUE_NAME in EXPR { ... }
```

`VALUE_NAME` and `KEY_NAME` introduce new locals in loop-body scope via desugaring to existing
`for` + local `var` declarations.

This SLP v1 only defines iteration for sequence-like expressions that support `len(x)` and `x[i]`.
Iterator protocol support for user-defined types is explicitly deferred.

## Motivation

Current SL loops require manual index management for common iteration patterns. This proposal adds a
direct iteration syntax while preserving explicit low-level semantics through desugaring.

Goals:

- reduce boilerplate for sequence iteration
- preserve deterministic evaluation and existing control-flow behavior
- keep implementation simple by lowering to existing `for` constructs

## Syntax

### Grammar additions

```ebnf
ForStmt         = "for" ( Block | Expr Block | ForClause Block | ForInClause Block ) .
ForInClause     = ForInValueBinding "in" Expr
                | ForInKeyBinding "," ForInValueBinding "in" Expr .
ForInKeyBinding = [ "&" ] Ident .
ForInValueBinding = "_" | [ "&" ] Ident .
```

Notes:

- `_` is only valid in value position.

## Semantic rules

### 1. Input category (v1 scope)

`EXPR` must typecheck as a sequence-like value supporting:

- `len(EXPR)` with unsigned integer result
- indexing `EXPR[i]` for `0 <= i < len(EXPR)`

Other input categories (maps, custom iterators, generator objects) are out of scope for this SLP.

### 2. Single evaluation of `EXPR`

`EXPR` is evaluated once before loop entry and stored in an implementation temporary.

Rationale: preserves ordinary `for` behavior and avoids repeated side effects from reevaluating
`EXPR` in condition/post/body.

### 3. Base lowering shape

For supported sequence inputs, lowering uses an index loop:

```sl
for var __sl_tmp_index uint = 0; __sl_tmp_index < len(__sl_tmp_seq); __sl_tmp_index += 1 {
    // synthesized bindings for key/value (if any)
    {
        // original body
    }
}
```

`__sl_tmp_seq` and `__sl_tmp_index` are implementation-generated temporaries that must not collide
with user names.

### 4. Value binding semantics

Given current element expression `<elem>`:

- `for value in expr { body }`
  lowers with `var value = <elem>`.
- `for &value in expr { body }`
  lowers with `var value = &<elem>` and has `&<elem>` type:
  - mutable source element -> `*T`
  - immutable source element -> `&T`
- `for _ in expr { body }`
  introduces no value binding and behaves as `for ... { body }` with only loop progression.

### 5. Key binding semantics

For `for key, value in expr { body }`, `<key>` is the iteration key.

- by value key: `var key = <key>` (or equivalent direct loop-index binding)
- by reference key: `for &key, value in expr { ... }` lowers as `var key = &<key>`

Constraint:

- `&key` is only valid when key type is non-synthetic.
- For v1 sequence lowering, key is synthetic (`uint` index), so `&key` is invalid.

### 6. Body scope

Key/value bindings are loop-body locals, equivalent to:

```sl
for ... {
    var <bindings...>
    {
        // user body
    }
}
```

This preserves existing shadowing and lifetime rules for block locals.

### 7. `_` in form 2

`for key, _ in expr { body }` is valid:

- key binding is created
- value is not loaded/bound

### 8. Control flow

`break`, `continue`, and `defer` behavior is unchanged because lowering targets existing `for`
semantics.

## Canonical examples

Example 1:

```sl
fn sum(items &[i32]) i32 {
    var accumulator i32
    for item in items {
        accumulator += item
    }
    return accumulator
}
```

is semantically equivalent to:

```sl
fn sum(items &[i32]) i32 {
    var accumulator i32
    for var __sl_tmp1 uint = 0; __sl_tmp1 < len(items); __sl_tmp1 += 1 {
        var item = items[__sl_tmp1]
        {
            accumulator += item
        }
    }
    return accumulator
}
```

Example 2:

```sl
fn sum(items &[i32]) i32 {
    var accumulator i32
    for index, item in items {
        accumulator += item * index as i32
    }
    return accumulator
}
```

is semantically equivalent to:

```sl
fn sum(items &[i32]) i32 {
    var accumulator i32
    for var index uint = 0; index < len(items); index += 1 {
        var item = items[index]
        {
            accumulator += item * index as i32
        }
    }
    return accumulator
}
```

Reference value capture:

```sl
for &item in items { ... }
```

desugars to:

```sl
for var __sl_tmp1 uint = 0; __sl_tmp1 < len(items); __sl_tmp1 += 1 {
    var item = &items[__sl_tmp1]
    {
        ...
    }
}
```

## Diagnostics

Recommended new diagnostics:

- `for_in_invalid_source`: source expression is not iterable under v1 sequence rules
- `for_in_key_ref_invalid`: `&key` used with synthetic key
- `for_in_value_binding_invalid`: invalid value binding prefix/shape
- `for_in_discard_key_invalid`: `_` used as key binding (not allowed)

## Compatibility and migration

This feature is additive and does not change existing `for` forms.

## Implementation notes

- parser: add `ForInClause` with required `in` marker
- typechecker: validate source supports `len` + indexing and bind synthesized key/value locals
- lowering/codegen: desugar to existing `for` form with temporaries
- formatter: preserve/normalize `for ... in` head spacing and comma rules

## Test plan

Add tests for:

1. Positive:
   - value-only iteration
   - key+value iteration
   - `&value` captures from mutable and immutable sources
   - `_` value discard in form 1 and form 2
2. Negative:
   - non-iterable source expression
   - `&key` on sequence source (synthetic key)
   - invalid binding forms (`for _, v in`, invalid prefix combinations)
3. Semantics:
   - `EXPR` evaluated once
   - scope/shadowing of key/value bindings
   - `continue`/`break` behavior matches equivalent lowered loop

## Non-goals

SLP-29 v1 does not add:

- user-defined iterator protocol
- map/dictionary iteration semantics
- ordered/unordered key traversal guarantees beyond sequence index order

## Follow-up work

Iterator protocol design for user-defined types is intentionally deferred to a separate proposal.
That proposal should define protocol surface, key/value ownership/reference behavior, and lowering
strategy without changing SLP-29 sequence semantics.
