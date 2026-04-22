# HEP-12 typed contexts and call-site context overlays (completed)

## Summary

HEP-12 introduces context-typed capability passing with two syntax additions:

- function context clause: `context <type>`
- call-site context clause: `with { ... }` and `with context`

Example:

```hop

struct AppContext {
    mem     mut&__hop_MemAllocator
    console u64
}

fn alloc() *i32 context AppContext {
    return new(i32)
}

fn say(msg str) context AppContext {
    print(msg)
}

fn run() context AppContext {
    var p = alloc()                  // implicit pass-through
    _ = p
    say("ok") with context           // explicit pass-through
    alloc() with { mem, console }    // call-local overlay
}

fn main() {
    run() // `main` forwards implicit root context (`mem`, `console`)
}
```

Design intent:

- clear authority surface at function boundaries
- readable call-sites with local overrides only where needed
- no ambient mutable global capability state

---

## Motivation

HopHop needs a unified way to pass capabilities and request-scoped values (allocator, filesystem,
process spawning, GUI handles, deadlines, cancellation tokens).

A typed context model keeps these values explicit and statically checked, while avoiding repetitive
manual parameter threading.

---

## Goals

- Compile-time checking of required context fields.
- Declaration-kind neutral design (works with current concrete types and future interfaces).
- Minimal syntax and predictable desugaring.
- Better readability than manually threading many capability parameters.

## Non-goals

- No dynamic/thread-local mutable context.
- No new declaration kind in HEP-12.
- No context-polymorphic generics in HEP-12.
- No block-form context rebinding construct in HEP-12.

---

## Syntax

### 1. Function context clause

```ebnf
FnDeclOrDef       = "fn" Ident "(" [ParamList] ")" [Type] [ContextClause] (";" | Block) ;
ContextClause     = "context" ContextType ;
ContextType       = Type ;
```

Notes:

- `context` is optional.
- `context` must resolve to a struct-shaped type.
- Named context types are supported directly in HEP-12.
- HEP-14 extends this with inline anonymous context shapes (`context { ... }`).

### 2. Call-site context clause

```ebnf
CallExprWithContext = CallExpr [WithContextClause] ;
WithContextClause   = "with" ("context" | ContextOverlay) ;
ContextOverlay      = "{" [ContextBindList] "}" ;
ContextBindList     = ContextBind { "," ContextBind } [","] ;
ContextBind         = Ident [ ":" Expr ] ;
```

Shorthand:

- `name` is equivalent to `name: context.name`.

Examples:

```hop
load("cfg") with { fs: sandboxFs("/app"), mem: arena }
show_ok() with context
```

---

## Semantics

### 1. Implicit `context` binding

A function with `context T` has an immutable local binding named `context` of type `T`.

Conceptual desugaring:

```hop
fn f(x i32) i32 context Ctx
```

behaves like:

```hop
fn f(x i32, __context mut&Ctx) i32
```

with `context` as a compiler-provided local alias for `__context`.

Exact lowering is implementation-defined.

### 2. Default call behavior

If caller and callee both use context and no `with` clause is present, the caller's current
`context` is forwarded automatically.

This is the primary noise-reduction rule. Calls such as `print("hello")` do not require explicit
`with` when current context already satisfies the callee.

### 3. Context compatibility

Context satisfaction is structural-by-field:

- Callee declares required fields via its context type.
- Effective call context must provide each required field by name with assignable type.
- Field names are part of the contract; matching by type alone is not sufficient.

This is declaration-kind neutral:

- today: fields may be concrete/named types
- future: fields may be interface types

No syntax changes are required for future interface support.

Example (name-sensitive check):

```hop

struct LogContext {
    tmpmem mut&Allocator
}

struct ExampleContext {
    mem mut&Allocator
}

fn log_event(msg str) context LogContext {
    _ = msg
}

fn example() context ExampleContext {
    log_event("hello") // error: missing required context field `tmpmem`
}
```

### 4. `with { ... }` overlay

`with { ... }` creates a call-local overlay from the caller's current context:

1. Start from caller context.
2. Apply listed field overrides.
3. Validate resulting context against callee requirements.
4. Use the result only for that call (no leakage outside the call expression).

Rules:

- Every bound field name must exist in caller context type.
- Duplicate field binds are invalid.
- Bare name shorthand (`mem`) means `mem = context.mem`.

### 5. `with context`

`with context` is explicit pass-through and is equivalent to omitting `with` for ordinary calls.
It exists for readability in places where explicitness helps.

### 6. Built-ins

Built-ins may require context fields.

Example policy:

- `print(...)` requires `console` in context.
- `new(T)` requires `mem` in context and desugars to allocation via `context.mem`.
- explicit forms such as `new(mem, T)` remain valid.

### 7. Entrypoint behavior

- Program entrypoint remains `fn main()` (no explicit `context` clause).
- `main` has an implicit root context with fields:
  - `mem` (platform allocator)
  - `console` (platform console handle/flags)

### 8. Declarations and overloads

- For a given function name/signature, all declarations and the definition must use the same
  `context` clause.
- `context` does not create a new overload dimension in HEP-12.

---

## Noise mitigation options

HEP-12 can stay strict while keeping code readable. Options:

1. Implicit forwarding by default (recommended)
   - Already in this HEP.
   - Only overrides require `with { ... }`.

2. Keep hot built-ins context-backed
   - `print`, logging, panic, and allocation read from `context` fields.
   - Avoids repetitive per-call wrappers.

3. Optional explicitness at boundaries
   - Use `with context` where reviewers want clear call-boundary authority threading.
   - Keep regular calls short in routine code.

4. Future sugar for repeated overrides (future HEP)
   - Optional block sugar such as `with { mem: arena } do { ... }`.
   - Pure sugar over repeated call-site overlays.

5. Narrow context types for library APIs
   - Libraries should declare small context shapes/types instead of `platform.Context`.
   - Reduces accidental authority spread.

---

## Examples

### Named context type

```hop
struct SaveContext {
    mem mut&__hop_MemAllocator
    fs  WriteFS
}

fn save(config mut&Config, filename str) ?error context SaveContext {
    return null
}
```

### Inline context shape

Requires HEP-14.

```hop
fn show_error(msg str) context { mem mut&__hop_MemAllocator, gui GUI } {
    _ = msg
}
```

### Override and pass-through

```hop
fn main() {
    print("hello")

    // Equivalent to: show_ok_message()
    // Idiomatic HopHop prefers omitting `with context` unless explicitness is needed.
    show_ok_message() with context
}
```

---

## Diagnostics

Implemented diagnostics:

- `CONTEXT_REQUIRED`
- `CONTEXT_MISSING_FIELD`
- `CONTEXT_TYPE_MISMATCH`
- `CONTEXT_UNKNOWN_FIELD`
- `CONTEXT_DUPLICATE_FIELD`
- `CONTEXT_CLAUSE_MISMATCH`
- `WITH_CONTEXT_ON_NON_CALL`

---

## Implementation notes

### Parser

- Add keywords `context` and `with`.
- Parse optional function `ContextClause`.
- Parse optional call-site `WithContextClause`.

### Typechecker

- Track current function context type.
- Auto-forward context on calls when no `with` is present.
- Build and typecheck overlay expressions for `with { ... }`.
- Validate field availability and assignability by name.

### Codegen

- Lower context as an implicit extra parameter (recommended: pointer/reference).
- Preserve call-local overlay semantics.
- Keep source-level behavior stable regardless of lowering details.

---

## Dependencies and staging

HEP-12 is designed to work immediately with named context types.

Optional adjacent proposals improve ergonomics but are not required to start:

- HEP-13: compound literals
- HEP-14: anonymous aggregate types
- HEP-15: struct field defaults

---

## Open questions

- Should future effect summaries be derived from context field usage for diagnostics/docs?
