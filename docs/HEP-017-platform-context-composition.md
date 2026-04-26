# HEP-17 platform context composition (completed)

## Status

Implemented (phase 1).

## Summary

HEP-17 defines a two-layer platform context model:

- `import "platform"` exposes a stable, target-agnostic `platform.Context`.
- Each concrete target exposes `platform/<target>.Context` that composes
  `platform.Context` via embedded-base struct composition.

Entrypoint `main` receives the concrete target context, while portable code can depend on the
stable base context.

Example:

```hop
// built-in package "platform"
pub struct Context {
    mem     mut&__hop_MemAllocator
    console i32
}
```

```hop
// package "platform/cli-libc"
import "platform"

pub struct Context {
    platform.Context
    stderr i32
}
```

---

## Motivation

HEP-12 currently gives `main` an implicit root context with fixed fields (`mem`, `console`).
That is too rigid for platforms that have different capability surfaces (for example GUI-only
hosts without stdin, or CLI hosts without GUI).

At the same time, replacing `platform` with a target-specific package would weaken portability
and make shared code harder to write.

HEP-17 keeps a stable portable baseline while allowing per-target extension.

---

## Goals

- Keep `import "platform"` stable and portable across targets.
- Allow each platform to define its concrete `main` context type.
- Preserve structural context compatibility from HEP-12.
- Keep platform-specific capabilities explicit in types.

## Non-goals

- Conditional imports in one translation unit (for example `if has_gui { import ... }`).
- Runtime capability discovery syntax.
- Context polymorphism or trait/interface-based capability abstraction.

---

## Design

### 1. Stable base context in `platform`

The `platform` package defines the baseline context contract:

- `mem` and `console` are required in all targets.
- Additional cross-target capabilities may be optional (for example `stdin`, `fs`).

Implemented now: `platform.Context` contains `mem` and `console`.
Optional fields (`stdin`, `fs`) are deferred.

### 2. Target context in `platform/<target>`

Each target provides:

```
lib/
    platform/
        <target>/
            context.hop
```

with:

```hop
import "platform"

pub struct Context {
    platform.Context
    // target-specific fields...
}
```

This uses HEP-6 embedded-base composition. As a result:

- `<target>.Context` is assignable to `platform.Context`.
- Portable functions can require `platform.Context`.
- Target-aware code can require `platform/<target>.Context`.

### 3. Entrypoint typing

`fn main()` remains source syntax.

The compiler binds `main`'s implicit `context` to the selected target context type:

- conceptually: `context platform/<target>.Context`
- lowering remains implementation-defined (current C backend may continue using
  `__hop_MainContext` as an internal ABI name).

### 4. Package roles

- `platform`: stable host boundary API and base context type.
- `platform/<target>`: target-specific context extension and target-specific helpers.

`import "platform"` remains a pseudo package.
`import "platform/<target>"` is a normal package import.

---

## Semantics with HEP-12

HEP-12 context checking remains unchanged.

Given:

```hop
import "platform"

fn alloc() *u8 context platform.Context {
    return new(u8)
}
```

and selected target `cli-libc`, `main` may call `alloc()` directly because
`platform/cli-libc.Context` structurally satisfies `platform.Context`.

For target-specific access:

```hop
import "platform/cli-libc" as cli

fn write_err(msg str) context cli.Context {
    _ = msg
    _ = context.stderr
}
```

---

## Portability model

HEP-17 intentionally keeps capability branching as a build-time concern:

- Put shared logic in portable packages requiring `platform.Context` or narrower custom contexts.
- Put host wiring in per-target entry packages (`app/main_cli`, `app/main_gui`, etc.).
- Select entry package by build target.

Single-file conditional import/capability branching is out of scope for this HEP.

### Potential reflection aid (HEP-18, optional)

HEP-18 reflection can be a mitigation for "what capabilities does this `main` context expose?"
by allowing compile-time inspection of `typeof(context)` (for example kind/field queries).

HEP-17 does not depend on HEP-18. The platform context composition model in this HEP stands on
its own even if reflection is never added.

---

## Migration

From current behavior:

- Existing programs using implicit `main` context keep working.
- Existing libraries that only require `mem`/`console` should migrate to explicit narrow
  context types or `platform.Context` where appropriate.
- Internal compiler/runtime representation of `__hop_MainContext` may remain as an implementation
  detail; source-level type authority moves to `platform.Context` and `platform/<target>.Context`.

---

## Implemented scope

1. `platform` pseudo package now exports `Context` and `exit`.
2. `platform.Context` currently is:

```hop
pub struct Context {
    mem     mut&__hop_MemAllocator
    console i32
}
```

3. `lib/platform/cli-libc/context.hop` exists and exports:

```hop
import "platform"

pub struct Context {
    platform.Context
    stderr i32
}
```

4. `hop` supports `--platform <target>` for `check`, `genpkg[:backend]`, `compile`, and `run`.
   Default target is `cli-libc`.
5. `import "platform/<target>"` is user-importable by default.
6. C ABI entrypoint remains `__hop_MainContext*` (ABI name unchanged), with runtime/layout updated
   for the current target shape.

## Deferred

1. Optional base fields such as `stdin` and `fs`.
2. Any prelude-based replacement for built-in host capability type names (for example
   `__hop_FileSystem`).
