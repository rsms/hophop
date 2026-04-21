# SL Examples

`fn main` is special and does not need to be marked `pub`.

Single-file mode is supported for package commands:

- `./_build/macos-aarch64-debug/slc checkpkg examples/single_file.sl`
- `./_build/macos-aarch64-debug/slc genpkg:c examples/single_file.sl`

## Core

- Basic function: `examples/basic.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/basic.sl`
- Declaration-order independence: `examples/order_independent.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/order_independent.sl`
- Strings, `assert`, `len`, `cstr`: `examples/strings.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/strings.sl`
- Full `str` capabilities (`&str`, `*str`, `[u8]` views, `concat`, `free`): `examples/str.sl`
  - Check: `./_build/macos-aarch64-debug/slc checkpkg examples/str.sl`
- Casts (`as`): `examples/casts.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/casts.sl`
- `defer`: `examples/defer.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/defer.sl`

## Types

- Struct/union/enum: `examples/aggregates.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/aggregates.sl`
- Anonymous struct/union types in fields, params, contexts, and inferred literals:
  `examples/anonymous_aggregates.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/anonymous_aggregates.sl`
- Pointers and arrays: `examples/pointers_arrays.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/pointers_arrays.sl`
- Variable-size struct (dependent trailing field): `examples/vss.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/vss.sl`
- SLP-2 references/slices (`*`, `[T]`, `*[T]`, slicing, implicit view conversions):
  `examples/slp2_refs_slices.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/slp2_refs_slices.sl`
- SLP-2 allocator `new` keyword forms: `examples/allocator.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/allocator.sl`
- Arena allocator with `import "mem"` and `free_all`: `examples/arena_allocator.sl`
  - Check: `./_build/macos-aarch64-debug/slc checkpkg examples/arena_allocator.sl`
- SLP-12 typed contexts/capabilities (`context`, `with { ... }`, `with context`):
  `examples/context.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/context.sl`
- Struct composition (embedded fields, promoted selectors): `examples/struct_composition.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/struct_composition.sl`
- Type functions (receiver sugar, `x.len()`): `examples/type_functions.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/type_functions.sl`
- Variadic functions (`...` params, forwarding, fixed+variadic calls): `examples/variadic.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/variadic.sl`
- `anytype` and `...anytype` (duck-typed field access, `typeof` specialization, const pack checks):
  `examples/anytype.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/anytype.sl`
- Type aliases (nominal alias + overload selection): `examples/type_alias.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/type_alias.sl`
- Generics (generic named types, inferred generic functions, receiver sugar, `type`-prefixed checks):
  `examples/generics.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/generics.sl`
- Reflection (`type`, `typeof`, `kind`, `base`): `examples/reflection.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/reflection.sl`
- Overload dispatch (same-name overloads): `examples/function_groups.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/function_groups.sl`
- SLP-13 compound literals (typed/inferred forms, nested init, ref binding, zero fill):
  `examples/compound-literals.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/compound-literals.sl`
- SLP-15 struct field defaults (sibling references, override behavior, evaluation order):
  `examples/struct-field-defaults.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/struct-field-defaults.sl`
- SLP-20 tagged unions (enum payload variants, switch narrowing, payload comparisons):
  `examples/tagged-union.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/tagged-union.sl`

## Control Flow

- `if`, `for`, `switch`, `break`, `continue`: `examples/control_flow.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/control_flow.sl`
- SLP-29 `for ... in` (value/ref/ptr capture, key+value, discard): `examples/for-in.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/for-in.sl`
- SLP-30 iterator protocol (`__iterator` + `next_*` hooks) for user-defined and infinite iterables:
  `examples/iterators.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/iterators.sl`
- Optionals (`?T`, `if optional`, `null`, unwrap `!`): `examples/optional.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/optional.sl`

## Imports

- Default alias from path tail:
  - App: `examples/imports-default/app`
  - Dependency: `examples/imports-default/lib/math`
  - Check package: `./_build/macos-aarch64-debug/slc checkpkg examples/imports-default/app`
  - Generate header: `./_build/macos-aarch64-debug/slc genpkg:c examples/imports-default/app`
- Explicit alias for non-identifier path tail:
  - App: `examples/imports-explicit/app`
  - Dependency: `examples/imports-explicit/lib/math-v2`
  - Check package: `./_build/macos-aarch64-debug/slc checkpkg examples/imports-explicit/app`

## Package Example

- App package: `examples/packages/app`
- Dependency package: `examples/packages/math`
- Package check: `./_build/macos-aarch64-debug/slc checkpkg examples/packages/app`
- Generate C header: `./_build/macos-aarch64-debug/slc genpkg:c examples/packages/app`
