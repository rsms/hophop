# HEP-40 anonymous functions

Status: Draft

## Summary

HEP-40 adds anonymous function expressions:

```hop
fn example() {
    f := fn(x int) int {
        return x * 2
    }
    assert call_int_fn(f, 3) == 6
}

fn call_int_fn(f fn(int) int, arg int) int {
    return f(arg)
}
```

Anonymous functions use the same parameter and result syntax as named functions. They produce
ordinary function-typed values and can be assigned, passed, returned, and called where their
function type is valid.

Anonymous functions are not closures. They may reference constants and package-scope variables, but
they must not reference local variables, parameters, or loop bindings from an enclosing runtime
scope. This keeps anonymous functions representable as ordinary function values without hidden
environment state.

The proposal also adds local named function declarations as block-scoped immutable function
bindings:

```hop
fn example() {
    fn double(x int) int {
        return x * 2
    }
    assert double(3) == 6
}
```

## Motivation

HopHop has function types, function values, generic helpers, and iterator-like APIs, but callers must
name every small callback. That is noisy for local transformations, test helpers, and APIs that take
one-off behavior.

Anonymous functions make these call sites direct while keeping allocation and lifetime behavior
explicit:

```hop
fn apply_ints(items *[int], f fn(int) int) {
    for i, item in items {
        items[i] = f(item)
    }
}

fn caller() {
    nums := [1, 2, 3]
    apply_ints(&nums, fn(n) {
        return n * 2
    })
}
```

## Goals

- Add `fn(...) { ... }` as an expression form.
- Add local named function declarations as block-scoped immutable function bindings.
- Reuse existing function parameter, result, and function-type syntax.
- Allow contextual parameter and result type inference for anonymous functions.
- Permit references to constants and package-scope variables.
- Reject references to enclosing local runtime bindings.
- Avoid heap allocation, reference counting, borrowed environments, or owned closure objects.

## Non-goals

HEP-40 does not add:

- capture-list syntax
- heap-allocated closures
- borrowed captures
- implicit copying or lifetime extension of enclosing locals
- partial application
- expression-bodied function literals
- generic anonymous functions with their own type parameter lists
- generic local named functions with their own type parameter lists
- anonymous functions as exported or foreign-linkage functions
- overloaded local function declarations
- forward references to local named functions, except self-recursion

## Syntax

Anonymous functions are primary expressions. Local named functions are statements:

```ebnf
Stmt             = ... | LocalFnDecl .
PrimaryExpr      = ... | AnonFnExpr .
AnonFnExpr       = "fn" "(" [ AnonFnParamList ] ")" [ FnResultClause ] Block .
LocalFnDecl      = "fn" Ident "(" [ AnonFnParamList ] ")" [ FnResultClause ] Block .
AnonFnParamList  = AnonFnParamGroup { "," AnonFnParamGroup } .
AnonFnParamGroup = "const" ( ( Ident | "_" ) [ Type ] | ( Ident | "_" ) "..." Type )
                 | ( Ident | "_" ) { "," ( Ident | "_" ) } [ Type ]
                 | ( Ident | "_" ) "..." Type .
```

The `fn` keyword already starts function declarations and function types. In expression context,
`fn(...) Block` is an anonymous function expression. In type context, `fn(...)` remains a function
type.

Examples:

```hop
var f fn(int) int = fn(x int) int {
    return x + 1
}

var g fn(int, int) int = fn(x, y) {
    return x * y
}

var h = fn() {
    trace("done")
}
```

Parameter types may be omitted only when an expected function type supplies them.

Local named function parameters require explicit types because the declaration has no expected
function type context:

```hop
fn example() {
    fn double(x int) int {
        return x * 2
    }
}
```

## Typing

### 1. No expected type

With no expected type, every anonymous function parameter must have an explicit type. An omitted
result type means `void`; result types are not inferred from `return` statements.

```hop
var f = fn(x int) int {
    return x + 1
}
```

If the result type is omitted, value-returning `return` statements are invalid:

```hop
f := fn(x int) {
    return x // invalid: omitted result means void
}
```

### 2. Expected function type

When the expected type is `fn(T1, ..., Tn) R`, omitted parameter types are filled from the expected
type and value-returning `return` statements must be assignable to `R`.

```hop
var f fn(x, y f32) f32
f = fn(x, y) {
    return x * y
}
```

The same rule applies in argument, return, assignment, and variable-initializer contexts:

```hop
fn make_fn() fn(int) int {
    return fn(x) {
        return x * 3
    }
}
```

### 3. Function type identity

The type of an anonymous function is the existing function type `fn(...) R`. Parameter names do not
participate in type identity. Assignability follows existing function-type rules after parameter and
result inference.

### 4. Generic calls

Anonymous functions can omit parameter and result types in generic calls only when the call
parameter has already resolved to a concrete `fn(...) R` type.

```hop
fn apply[T](items *[T], f fn(T) T) {
    for i, item in items {
        items[i] = f(item)
    }
}

fn caller() {
    nums := [1, 2, 3]
    apply(&nums, fn(n) {
        return n * 2
    })
}
```

Unconstrained generic parameters such as `F` do not provide an expected function type. In that case,
anonymous function parameter and result types must be explicit.

### 5. Const initializers

Anonymous functions are valid in `const` initializers exactly when existing named function values
are valid const-like values in the implementation.

```hop
const double = fn(x int) int {
    return x * 2
}
```

If named function values are not accepted as const initializers, anonymous function values are also
not accepted. This keeps HEP-40 from adding a special const category just for anonymous functions.

### 6. Local named functions

A local named function declaration creates an immutable local binding whose value has the
corresponding function type.

```hop
fn example() {
    fn double(x int) int {
        return x * 2
    }
    assert double(21) == 42
}
```

Local named functions:

- are visible from their declaration point to the end of the containing block
- may refer to their own name from inside their body for direct recursion
- do not overload by signature
- cannot be forward-referenced before their declaration
- use the same no-closure name-resolution rules as anonymous functions

This is equivalent to an immutable local function binding, except that the function name is available
inside its own body.

## Name resolution and lifetime

Anonymous functions and local named functions may reference:

- package-level variables and constants
- constants in any visible scope

They must not reference local variables, parameters, or loop bindings from an enclosing runtime
scope. This rule applies even when the function value is called immediately.

Constants are allowed because they do not require runtime environment storage. Package-scope
variables are allowed because their storage has static lifetime.

Valid examples:

```hop
var package_level_value int

fn reference_top_level() fn() int {
    return fn() int {
        package_level_value += 1
        return package_level_value
    }
}

fn reference_const() fn(int) &str {
    const names = ["Anne", "Baron", "Cat"]
    return fn(n int) &str {
        return names[n % len(names)]
    }
}

fn local_named_function() {
    const factor = 2
    fn double(x int) int {
        return x * factor
    }
    assert double(3) == 6
}
```

Invalid examples:

```hop
fn error_reference_local() {
    x := 3
    f := fn() int {
        return x // invalid: anonymous function references enclosing local `x`
    }
    assert f() == 3
}

fn error_reference_param(x int) fn() int {
    return fn() int {
        return x // invalid: anonymous function references enclosing parameter `x`
    }
}

fn error_reference_inner_scope() {
    var f fn() int
    {
        x := 123
        f = fn() int {
            return x // invalid: anonymous function references enclosing local `x`
        }
    }
    assert f() == 123
}

fn error_local_named_reference_local() {
    x := 3
    fn value() int {
        return x // invalid: local named function references enclosing local `x`
    }
    assert value() == x
}

fn error_local_named_reference_param(x int) {
    fn value() int {
        return x // invalid: local named function references enclosing parameter `x`
    }
    assert value() == x
}
```

## Explicit callback state

HEP-40 does not add closures. Code that needs callback state should model it as ordinary data plus a
function field:

```hop
// thing_package/thing.hop
pub struct Callback {
    f    fn(data rawptr) int
    data rawptr
}

pub struct Thing {
    callback Callback
}

pub fn set_callback(thing *Thing, callback Callback) {
    thing.callback = callback
}

pub fn call(callback Callback) int {
    return callback.f(callback.data)
}

// my_program.hop
import "thing_package" { Thing, Callback, set_callback, call }

fn example(thing *Thing) {
    callback := Callback{
        f: fn(data rawptr) int {
            n := data as int
            return n
        },
        data: 123 as rawptr,
    }
    thing.set_callback(callback)
}

fn main() {
    thing := Thing{}
    example(&thing)
    assert thing.callback.call() == 123
}
```

This keeps ownership visible in the program instead of hidden in a closure allocation.

## Lowering

Each anonymous function and local named function is assigned a deterministic internal symbol derived
from its package, source location, and containing function.

They lower like package-level functions and can use the existing function-value representation.

Because they cannot capture enclosing runtime bindings, no environment object is required. Backend
lowering does not need hidden parameters or adapter state.

## Diagnostics

Recommended diagnostics:

- `anon_fn_param_type_required`: anonymous function parameter requires an explicit type
- `anon_fn_return_type_mismatch`: anonymous function return value is not assignable to expected
  result type
- `anon_fn_return_inference_failed`: anonymous function result type cannot be inferred
- `anon_fn_capture_forbidden`: anonymous function cannot reference enclosing runtime binding
- `anon_fn_const_initializer_invalid`: anonymous function cannot be used in a `const` initializer
  when named function values are not valid const values
- `anon_fn_generic_params_forbidden`: anonymous and local named functions cannot declare type
  parameters
- `local_fn_duplicate`: duplicate local named function in the same scope
- `local_fn_forward_reference`: local named function cannot be referenced before its declaration

## Implementation notes

### Parser

- Add an anonymous-function expression node.
- Parse `fn(...) Block` in primary-expression context.
- Parse `fn Ident(...) Block` as a local function declaration in statement context.
- Reuse existing parameter-list and result-clause parsing where possible.
- Permit missing parameter types only on anonymous function parameters.

### Type checker

- Thread expected function type into anonymous function checking.
- Infer omitted parameter types from expected parameter types.
- Infer result type when no expected result exists.
- Resolve names inside anonymous functions using ordinary lexical lookup.
- Reject references to enclosing local variables, parameters, and loop bindings.
- Permit constants and package-scope variables.
- Bind local named functions as immutable locals with no overload set.
- Allow a local named function to reference itself inside its own body.
- Reject forward references to local named functions from preceding statements.
- Allow anonymous functions in `const` initializers only when named function values already satisfy
  the implementation's const-initializer rules.

### Backends

- Emit deterministic helper functions for anonymous functions and local named functions.
- Preserve existing direct function-pointer lowering.

## Test plan

1. Positive:
   - assign anonymous function to a local
   - pass anonymous function to a function-typed parameter
   - return anonymous function from a function
   - infer parameter types from assignment, argument, and return context
   - infer result type without expected result type
   - reference package-level variable from returned anonymous function
   - reference local constant from returned anonymous function
   - anonymous function in `const` initializer when named function values are valid const values
   - local named function declaration
   - recursive local named function declaration
   - model callback state with explicit data pointer plus function field
2. Negative:
   - omitted parameter type without expected function type
   - omitted parameter type in local named function
   - incompatible return values without expected result type
   - return value not assignable to expected result type
   - anonymous function references enclosing local variable
   - anonymous function references enclosing parameter
   - anonymous function references enclosing loop binding
   - local named function references enclosing local variable or parameter
   - local named function forward reference
   - duplicate local named function in same scope
   - generic anonymous or local named function type parameter list rejected
   - anonymous function in `const` initializer when named function values are not valid const values
3. Regression:
   - named function declarations still parse as declarations
   - `fn(...)` in type context still parses as a function type
   - existing package-level function values still lower as before
