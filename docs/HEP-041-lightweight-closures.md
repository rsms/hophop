# HEP-41 lightweight stack closures

Status: Draft

## Summary

HEP-41 extends HEP-40 anonymous functions and local named functions with lightweight closures.
Closures may capture enclosing runtime locals when the resulting function value does not outlive
those locals.

Function values use a uniform fat-pointer representation:

```hop
struct function_value {
    code rawptr
    data rawptr
}
```

Conceptually, every indirect function-value call passes the `data` pointer as a hidden final
argument:

```hop
f.code(arg0, arg1, ..., f.data)
```

Non-capturing ordinary functions use the same representation when materialized as function values.
Their function-value entry accepts the hidden `data rawptr` parameter and ignores it.

Capturing closures are stack closures in this proposal. The compiler places captured locals in a
compiler-generated closure struct in the enclosing function. The function value stores a pointer to
that struct in `data`.

## Motivation

HEP-40 deliberately made anonymous functions non-closures. That keeps function values simple, but it
also makes common callback patterns awkward:

```hop
fn use_items(items [int], keep fn(int) bool) {
    for item in items {
        if keep(item) {
            // ...
        }
    }
}

fn main() {
    seen := Set[int]{}
    use_items(get_items(), fn(item) bool {
        return seen.intern(item)
    })
}
```

The callback needs local state. Requiring users to manually construct a context object and pass a
function plus `rawptr` makes ordinary callback code too low-level.

HEP-41 aims to provide closures for local, non-escaping callback use while preserving a simple ABI
and avoiding heap allocation.

## Goals

- Let anonymous functions and local named functions capture enclosing runtime locals.
- Support mutation of captured locals.
- Keep `fn(...) R` as one uniform function-value type for capturing and non-capturing functions.
- Represent function values as `(code, data)` fat pointers.
- Pass `data rawptr` as a hidden final argument for function-value calls.
- Keep closure storage stack-allocated in the enclosing function.
- Reject closure values that may outlive captured locals.
- Allow closure values to be passed to parameters proven not to escape.

## Non-goals

HEP-41 does not add:

- heap-allocated closures
- reference-counted or garbage-collected closure environments
- returning closures that capture stack locals
- storing stack closures in top-level variables
- storing stack closures in heap objects or other escaping storage
- implicit lifetime extension of captured locals
- capture-list syntax
- partial application
- non-escaping parameter syntax
- concurrent execution or async lifetime rules

## Function value representation

All values of function type are represented as a pair:

```hop
struct _fn_value {
    code rawptr
    data rawptr
}
```

This is a representation rule, not a source-level struct declaration. Existing source-level
function types remain unchanged:

```hop
fn(int) int
fn(str, bool)
```

The representation is uniform. A variable with function type can hold any compatible function value:

```hop
var f fn(int) int

f = top_level_function
f = fn(x int) int { return x + 1 }
f = local_capturing_function
```

The `code` pointer targets a function-value entry. The entry has the source function parameters plus
a hidden final `data rawptr` parameter.

For a source type:

```hop
fn(A, B) R
```

the lowered function-value entry shape is:

```hop
fn(A, B, data rawptr) R
```

The hidden parameter is never visible to source code and does not participate in function type
identity.

## Ordinary functions as values

Ordinary non-capturing functions have `data = null` when converted to function values.

```hop
fn double(x int) int {
    return x * 2
}

fn main() {
    f := double
    assert f(21) == 42
}
```

When `double` is used as a function value, the compiler provides a function-value entry with the
hidden `data rawptr` parameter:

```hop
fn double$value(x int, data rawptr) int {
    return double(x)
}

f := (double$value, null)
```

An implementation may instead compile the ordinary function body directly with an ignored hidden
`data rawptr` parameter when that is cheaper for the backend. Direct calls that are statically known
not to go through a function value may still lower to the backend's normal direct-call convention.

## Capturing locals

When a function captures runtime locals from an enclosing function, the compiler gives the closure
entry access to those locals through a compiler-generated closure struct. A backend may move captured
locals into that struct, or may store pointers to the original local storage when that preserves the
same lifetime and mutation behavior.

Source:

```hop
fn main() {
    x := 1
    y := 2
    fn f(z int) int {
        x += 1
        return x + y + z
    }
    assert f(3) == 7
}
```

Conceptual lowering:

```hop
struct _main_f_closure {
    x int
    y int
}

fn main() {
    f_closure := _main_f_closure{ x: 1, y: 2 }

    f := (_main_f$value, &f_closure as rawptr)
    assert f(3) == 7
}

fn _main_f$value(z int, data rawptr) int {
    cl := data as *_main_f_closure
    cl.x += 1
    return cl.x + cl.y + z
}
```

This lowering preserves mutation: `x += 1` mutates the captured storage, not a stale copy.

If a captured local is also used directly in the enclosing function after the closure is formed, the
direct uses also refer to the closure field:

```hop
fn main() {
    x := 1
    f := fn() int {
        x += 1
        return x
    }
    assert f() == 2
    assert x == 2
}
```

Conceptually, once `x` is captured, the closure and the enclosing function share the same logical
storage for `x` for the remainder of its scope. The source name `x` continues to behave like a local
variable.

## Capture eligibility

Anonymous functions and local named functions may capture:

- enclosing runtime locals
- enclosing parameters
- loop bindings whose lifetime is proven to contain the closure value use

They may also reference package-scope variables and visible constants as in HEP-40.

Captured bindings are captured by storage location, not by immutable value. Mutating a captured
binding inside the closure mutates the same logical local observed by the enclosing function.

The compiler rejects captures that would require unsupported storage:

- captures of locals whose address or representation cannot be moved into a closure struct
- captures crossing unsupported control-flow or lifetime boundaries
- captures in closures that may escape the captured storage lifetime

## Rule 1: non-escaping stack closures

Capturing closure values are stack closures. A stack closure value must not outlive the activation
record that owns its closure struct.

The first version uses a conservative rule:

- A capturing closure may be called directly.
- A capturing closure may be assigned to a local whose scope is not wider than the captured locals.
- A capturing closure may be passed to a function parameter when the compiler can prove that the
  parameter does not escape inside the callee.
- A capturing closure may not be returned.
- A capturing closure may not be assigned to a top-level `var` or `const`.
- A capturing closure may not be stored in heap storage.
- A capturing closure may not be stored through a pointer, reference, mutable slice, or aggregate
  field unless the compiler can prove that storage is non-escaping.
- A capturing closure may not be assigned into a local declared outside the captured locals' scope.

This rule is intentionally stricter than necessary. Future HEPs may loosen it with more precise
escape analysis.

## Inferred parameter escape analysis

APIs that call a callback without storing it do not need new syntax. They use ordinary function
parameters:

```hop
fn get_items(keep fn(Item) bool) {
    // `keep` is called but not stored or returned.
}
```

The compiler computes a conservative escape summary for function parameters. A capturing stack
closure may be passed to a parameter only when that parameter is proven non-escaping.

Inside a function, a function parameter escapes if it is:

- returned
- stored in top-level storage
- assigned to a function-typed local that may outlive the call
- stored in heap storage or through pointer/reference storage
- stored in an aggregate field that may escape
- passed to another function parameter that is not proven non-escaping

Calling a function parameter is non-escaping. Passing it to another function is non-escaping only
when the target parameter is also proven non-escaping. Passing it to an unknown or foreign function
is escaping by default unless the compiler has explicit metadata proving otherwise.

This lets stack closures flow through callback APIs without requiring heap allocation or new syntax:

```hop
fn get_items(keep fn(int) bool) {
    for item in items() {
        if keep(item) {
            // ...
        }
    }
}

fn main() {
    seen := Set[int]{}
    get_items(fn(item) bool {
        return seen.intern(item)
    })
}
```

Parameter escape summaries are part of checked package metadata. If a package changes a parameter
from non-escaping to escaping, callers that pass stack closures may stop compiling. This is a normal
type-checking consequence of changing the callee's implementation contract.

## Direct local calls

A local named function that captures locals may still be called by name:

```hop
fn main() {
    x := 5
    fn example(y int) int {
        return x * y
    }
    assert example(3) == 15
}
```

The compiler may lower `example(3)` either as a direct call to the closure entry with the known
closure pointer or by materializing the function value and calling it indirectly. Both are
semantically equivalent.

## Function value calls

Calling a function value always uses the fat-pointer call path:

```hop
f := example
assert f(3) == 15
```

Conceptual lowering:

```hop
f := (_main_example$value, &example_closure as rawptr)
assert f.code(3, f.data) == 15
```

Uniform function call syntax remains unchanged. Receiver-call sugar still treats the function value
as the first argument:

```hop
fn apply_twice(f fn(int) int, x int) int {
    return f(f(x))
}

fn main() {
    shift := fn(x int) int {
        return x + 10
    }
    assert shift.apply_twice(3) == 23
}
```

## Closure struct layout

The compiler owns closure struct layout. Source code cannot name or inspect these structs.

Required properties:

- one field per captured binding, unless optimized away
- fields preserve the source binding's type and mutability behavior
- captured function values may themselves be fields
- closure structs are initialized before the closure value is used
- closure structs have a lifetime no shorter than all accepted uses of the closure value

Backends may choose different physical representations when semantics are preserved. Examples:

- stack struct plus `rawptr` data
- split scalar locals when the closure is only directly called
- backend-native closure records
- specialized direct-call lowering that avoids materializing the fat pointer

The source-level ABI for function values remains `(code, data)`.

## Nested closures

A closure may capture a local that is already stored in an enclosing closure struct. The nested
closure captures the same logical storage location, not an independent copy.

```hop
fn main() {
    x := 0
    outer := fn() {
        inner := fn() {
            x += 1
        }
        inner()
    }
    outer()
    assert x == 1
}
```

The compiler may represent this by giving the nested closure a pointer to the outer closure struct
or by flattening the captured fields into a new closure struct, as long as mutation aliases the same
logical local.

## Diagnostics

New diagnostics should cover:

- capturing closure escapes its local lifetime
- assigning capturing closure to top-level storage
- returning capturing closure
- storing capturing closure in escaping storage
- passing capturing closure to an escaping parameter
- storing function parameter in escaping storage
- unsupported capture representation

The diagnostic should point at the closure expression or local function declaration and, when useful,
add a note at the captured binding.

## Implementation notes

### Parser and AST

No syntax change is needed for captures themselves. HEP-40 anonymous functions and local named
functions become capture-capable.

No parameter syntax is added for non-escaping callbacks. Ordinary `fn(...) R` parameter syntax is
unchanged.

### Type checking

The typechecker must:

1. Resolve identifiers in closure bodies using lexical lookup.
2. Build a capture set for each anonymous or local function.
3. Classify captures as mutable storage captures.
4. Rewrite captured locals into closure-storage locals for the enclosing scope.
5. Track whether each function value is capturing.
6. Apply conservative escape checks.
7. Compute and consume function-parameter escape summaries across package boundaries.

The first implementation may reject complex cases rather than silently lowering them incorrectly.

### Lowering

For each capturing function, lowering creates:

- a closure struct type
- a closure storage local in the enclosing function
- a function-value entry with hidden `data rawptr`
- casts from `data` to the closure struct pointer inside the entry
- field loads/stores for captured bindings

For non-capturing ordinary functions used as values, lowering creates or reuses a function-value
entry that accepts and ignores hidden `data rawptr`.

### Backends

Different backends may choose different implementation strategies, but all must preserve:

- uniform function-value representation
- hidden `data rawptr` on function-value calls
- stack lifetime restrictions
- mutation-visible captures

The C backend can lower closure structs to static generated C struct types and function-value
entries to `static` helper functions.

The evaluator and MIR backends can either model the fat pointer directly or use a richer internal
function reference value as long as observable behavior matches.

## Tests

Positive tests:

- capture immutable local and call directly
- capture mutable local, mutate inside closure, observe mutation after call
- assign capturing closure to a local and call it
- pass capturing closure to a parameter proven non-escaping
- nested closure mutates an outer captured local
- non-capturing ordinary function assigned to `fn(...)` value still works
- receiver-call sugar works with capturing function values

Negative tests:

- return capturing closure
- assign capturing closure to top-level variable
- store capturing closure in aggregate that escapes
- store capturing closure through pointer/reference storage
- pass capturing closure to ordinary escaping `fn(...)` parameter
- store function parameter in escaping storage and reject stack-closure callers
- capture unsupported local representation

## Open questions

- Whether function-value ABI should place `data rawptr` first or last at the backend ABI level.
- Whether direct calls to functions that may be used as values should share the hidden-parameter
  entry or use a separate direct-call entry plus thunk.
- Whether future heap closures should be a distinct type or an allowed escape mode for the same
  `fn(...) R` type.
