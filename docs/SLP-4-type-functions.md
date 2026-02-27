# SLP-4 type functions (completed)

## Summary

SLP-4 adds type-function call syntax as sugar:

```sl
f(x, a, b)
x.f(a, b)
```

Both forms are equivalent and resolve to the same target function.

Dispatch is compile-time only. There is no runtime dispatch table.

---

## Motivation

SL already uses package-level functions as its core abstraction. This improves call-site ergonomics without introducing methods as a new declaration kind.

Goals:
- make call sites read naturally (`x.update(...)`)
- preserve package-level function model
- support deterministic overloading
- keep lowering simple and static

Non-goal:
- interfaces, traits, or runtime method dispatch

---

## Syntax

### Selector call sugar

No new expression syntax is required. Existing selector+call syntax gains new meaning when selector resolution does not produce a field:

```sl
expr.name(args...)
```

It can resolve as a type-function call to `name(expr, args...)`.

### EBNF additions

```ebnf
TopDecl            = ["pub"] (StructDecl | UnionDecl | EnumDecl | FnDeclOrDef | ConstDecl) ;
```

---

## Core semantics

### 1. Desugaring model

A selector-call expression

```sl
E.f(A1, ..., An)
```

is resolved during typecheck as if it were

```sl
f(E, A1, ..., An)
```

with these guarantees:
- `E` is evaluated exactly once
- argument evaluation order is left-to-right
- side effects are preserved

This is semantic equivalence, not required AST rewriting.

### 2. Field precedence

Selector resolution order for `E.f(...)`:
1. resolve `f` as a field (including promoted fields via struct composition)
2. if field exists, use field expression result; normal call rules then apply
3. only if no field is found, attempt type-function resolution

### 3. Call-form-only in v1

`E.f` without `(...)` is not a type-function value.

Only call form participates in type-function resolution.

### 4. Package lookup

- `E.f(...)` resolves `f` in current package scope.
- `pkg.f(...)` resolves directly in imported package `pkg` export scope.
- `E.f(...)` does not perform implicit cross-package lookup.

---

## Multiple dispatch model

SLP-4 resolves overloads from visible functions sharing the target call name.

### Dispatch and ranking

Given call name `N` and argument list `(a0, a1, ... ak)`:
- unqualified call: candidates are functions named `N`
- selector-call: same candidate set, using receiver-injected arguments

Candidate filtering:
- same arity
- each argument assignable to corresponding parameter type

If no candidates remain: `NO_MATCHING_OVERLOAD`.

If one candidate remains: select it.

If multiple remain, rank using per-parameter conversion costs (lower is better):
- `0`: exact type match
- `1`: mutability relaxation (`mut&T -> &T`, `mut[S] -> [S]`)
- `2`: embedded-base upcast (`Derived -> Base`, including refs)
- `3`: untyped literal coercion (`untyped_int/float`)
- `4`: optional lift (`T -> ?T`)

Comparison:
- lexicographic compare of per-parameter cost vector
- then total-cost compare
- if still tied: `AMBIGUOUS_CALL`

---

## Built-in participation

Built-in functions participate in selector-call sugar when the call shape matches.

At minimum:
- `x.len()` => `len(x)`
- `ma.new(T[, N])` => `new(ma, T[, N])`
- `s.cstr()` => `cstr(s)`
- `msg.print()` => `print(msg)`
- `msg.panic()` => `panic(msg)`

Built-ins that are not receiver-first forms (for example `sizeof(...)`) do not participate.

---

## Examples

### Basic type-function call

```sl
struct Foo {}
fn something(f &Foo, n i32)

fn example(f &Foo) {
    something(f, 123)
    f.something(123)
}
```

### Overloaded receiver sugar

```sl
struct Pet {}
struct Spaceship {}

fn update(pet mut&Pet, timestamp u64)
fn update(ship mut&Spaceship)

fn example(pet mut&Pet, ship mut&Spaceship, timestamp u64) {
    pet.update(timestamp)
    ship.update()
}
```

---

## Diagnostics

- `NO_MATCHING_OVERLOAD`: name exists but no viable candidate for argument types
- `AMBIGUOUS_CALL`: multiple best candidates after ranking

`slc` should print a short candidate list for overload errors (bounded length).

---

## Interaction with SLP-6 struct composition

Embedded-base upcasts are part of dispatch ranking.

Rules:
- exact receiver type match beats base-upcast match
- derived-to-base transitive conversions are allowed as assignability candidates
- if two candidates are equal after ranking, emit `AMBIGUOUS_CALL`

---

## Lowering and codegen

No runtime mechanism is added.

After typecheck selects a concrete function, codegen emits a direct call to that function.

For selector sugar, codegen emits the resolved target with receiver inserted as first argument.

---

## Non-goals

SLP-4 does not add:
- methods declared inside `struct`
- method sets or method namespaces
- interface/trait dispatch
- function-value partial application from selector form (`x.f`)
