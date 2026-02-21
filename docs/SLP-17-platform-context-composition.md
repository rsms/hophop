# SLP-17 platform context composition

## Summary

SLP-17 defines a two-layer platform context model:

- `import "platform"` exposes a stable, target-agnostic `platform.Context`.
- Each concrete target exposes `platform/<target>.Context` that composes
  `platform.Context` via embedded-base struct composition.

Entrypoint `main` receives the concrete target context, while portable code can depend on the
stable base context.

Example:

```sl
// built-in package "platform"
pub struct Context {
    mem     &__sl_MemAllocator
    console i32
    stdin   ?i32
    fs      ?__sl_FileSystem
}
```

```sl
// package "platform/cli-libc"
import "platform"

pub struct Context {
    platform.Context
    stderr i32
}
```

---

## Motivation

SLP-12 currently gives `main` an implicit root context with fixed fields (`mem`, `console`).
That is too rigid for platforms that have different capability surfaces (for example GUI-only
hosts without stdin, or CLI hosts without GUI).

At the same time, replacing `platform` with a target-specific package would weaken portability
and make shared code harder to write.

SLP-17 keeps a stable portable baseline while allowing per-target extension.

---

## Goals

- Keep `import "platform"` stable and portable across targets.
- Allow each platform to define its concrete `main` context type.
- Preserve structural context compatibility from SLP-12.
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

The exact optional field set can evolve, but should stay small and conservative.
In this draft, `fs` uses built-in type `__sl_FileSystem`.
Future work may move such global types to a concrete `builtin.sl`.

### 2. Target context in `platform/<target>`

Each target provides:

```
lib/
    platform/
        <target>/
            context.sl
```

with:

```sl
import "platform"

pub struct Context {
    platform.Context
    // target-specific fields...
}
```

This uses SLP-6 embedded-base composition. As a result:

- `<target>.Context` is assignable to `platform.Context`.
- Portable functions can require `platform.Context`.
- Target-aware code can require `platform/<target>.Context`.

### 3. Entrypoint typing

`fn main()` remains source syntax.

The compiler binds `main`'s implicit `context` to the selected target context type:

- conceptually: `context platform/<target>.Context`
- lowering remains implementation-defined (current C backend may continue using
  `__sl_MainContext` as an internal ABI name).

### 4. Package roles

- `platform`: stable host boundary API and base context type.
- `platform/<target>`: target-specific context extension and target-specific helpers.

`import "platform"` remains a pseudo package.
`import "platform/<target>"` is a normal package import.

---

## Semantics with SLP-12

SLP-12 context checking remains unchanged.

Given:

```sl
import "platform"

fn alloc() *u8 context platform.Context {
    return new(u8)
}
```

and selected target `cli-libc`, `main` may call `alloc()` directly because
`platform/cli-libc.Context` structurally satisfies `platform.Context`.

For target-specific access:

```sl
import "platform/cli-libc" as cli

fn write_err(msg str) context cli.Context {
    _ = msg
    _ = context.stderr
}
```

---

## Portability model

SLP-17 intentionally keeps capability branching as a build-time concern:

- Put shared logic in portable packages requiring `platform.Context` or narrower custom contexts.
- Put host wiring in per-target entry packages (`app/main_cli`, `app/main_gui`, etc.).
- Select entry package by build target.

Single-file conditional import/capability branching is out of scope for this SLP.

### Potential reflection aid (SLP-18, optional)

SLP-18 reflection can be a mitigation for "what capabilities does this `main` context expose?"
by allowing compile-time inspection of `typeof(context)` (for example kind/field queries).

SLP-17 does not depend on SLP-18. The platform context composition model in this SLP stands on
its own even if reflection is never added.

---

## Migration

From current behavior:

- Existing programs using implicit `main` context keep working.
- Existing libraries that only require `mem`/`console` should migrate to explicit narrow
  context types or `platform.Context` where appropriate.
- Internal compiler/runtime representation of `__sl_MainContext` may remain as an implementation
  detail; source-level type authority moves to `platform.Context` and `platform/<target>.Context`.

---

## Implementation plan

1. Add source for base context in the `platform` package API surface.
2. Add target context package layout under `lib/platform/<target>/context.sl`.
3. Extend target selection plumbing so `main` context type resolves to
   `platform/<target>.Context`.
4. Keep current generated C ABI stable initially, mapping selected target context to
   internal `__sl_MainContext` during lowering.
5. Add tests:
   - `main` context is target-selected.
   - target context upcasts to `platform.Context`.
   - portable function requiring `platform.Context` callable from `main`.
   - target-specific field use requires importing `platform/<target>`.

---

## Open questions

1. Should `platform/<target>` be user-importable by default, or gated behind a build flag for
   strict portability mode?
2. Should optional base fields be standardized now (`stdin`, `fs`) or introduced incrementally?
3. Should `__sl_FileSystem` remain a built-in type, or be replaced by a `builtin.sl`-defined
   global type once prelude support is added?
