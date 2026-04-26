# HopHop Examples

`fn main` is special and does not need to be marked `pub`.

Single-file mode is supported for package commands:

- `./_build/macos-aarch64-debug/hop check examples/single_file.hop`
- `./_build/macos-aarch64-debug/hop genpkg:c examples/single_file.hop`

## Core

- Basic function: `examples/basic.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/basic.hop`
- Declaration-order independence: `examples/order_independent.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/order_independent.hop`
- Strings, `assert`, `len`, `cstr`: `examples/strings.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/strings.hop`
- Full `str` capabilities (`&str`, `*str`, `[u8]` views, `concat`, `free`): `examples/str.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/str.hop`
- Casts (`as`): `examples/casts.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/casts.hop`
- `defer`: `examples/defer.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/defer.hop`

## Types

- Struct/union/enum: `examples/aggregates.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/aggregates.hop`
- Anonymous struct/union types in fields, params, contexts, and inferred literals:
  `examples/anonymous_aggregates.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/anonymous_aggregates.hop`
- Pointers and arrays: `examples/pointers_arrays.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/pointers_arrays.hop`
- Variable-size struct (dependent trailing field): `examples/vss.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/vss.hop`
- HEP-2 references/slices (`*`, `[T]`, `*[T]`, slicing, implicit view conversions):
  `examples/hep2_refs_slices.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/hep2_refs_slices.hop`
- HEP-2 allocator `new` keyword forms: `examples/allocator.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/allocator.hop`
- Arena allocator with `import "mem"` and `free_all`: `examples/arena_allocator.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/arena_allocator.hop`
- HEP-12 typed contexts/capabilities (`context`, `context { ... }`, `context context`):
  `examples/context.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/context.hop`
- Struct composition (embedded fields, promoted selectors): `examples/struct_composition.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/struct_composition.hop`
- Type functions (receiver sugar, `x.len()`): `examples/type_functions.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/type_functions.hop`
- Variadic functions (`...` params, forwarding, fixed+variadic calls): `examples/variadic.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/variadic.hop`
- Named arguments (labels, shorthand, reordering, underscore positional prefix):
  `examples/named_arguments.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/named_arguments.hop`
- `anytype` and `...anytype` (duck-typed field access, `typeof` specialization, const pack checks):
  `examples/anytype.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/anytype.hop`
- Type aliases (nominal alias + overload selection): `examples/type_alias.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/type_alias.hop`
- Generics (generic named types, inferred generic functions, receiver sugar, `type`-prefixed checks):
  `examples/generics.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/generics.hop`
- Reflection (`type`, `typeof`, `kind`, `base`): `examples/reflection.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/reflection.hop`
- Overload dispatch (same-name overloads): `examples/function_groups.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/function_groups.hop`
- HEP-13 compound literals (typed/inferred forms, nested init, ref binding, zero fill):
  `examples/compound-literals.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/compound-literals.hop`
- HEP-15 struct field defaults (sibling references, override behavior, evaluation order):
  `examples/struct-field-defaults.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/struct-field-defaults.hop`
- HEP-20 tagged unions (enum payload variants, switch narrowing, payload comparisons):
  `examples/tagged-union.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/tagged-union.hop`

## Control Flow

- `if`, `for`, `switch`, `break`, `continue`: `examples/control_flow.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/control_flow.hop`
- HEP-29 `for ... in` (value/ref/ptr capture, key+value, discard): `examples/for-in.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/for-in.hop`
- HEP-30 iterator protocol (`__iterator` + `next_*` hooks) for user-defined and infinite iterables:
  `examples/iterators.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/iterators.hop`
- Optionals (`?T`, `if optional`, `null`, unwrap `!`): `examples/optional.hop`
  - Check: `./_build/macos-aarch64-debug/hop check examples/optional.hop`

## Imports

- Default alias from path tail:
  - App: `examples/imports-default/app`
  - Dependency: `examples/imports-default/lib/math`
  - Check package: `./_build/macos-aarch64-debug/hop check examples/imports-default/app`
  - Generate header: `./_build/macos-aarch64-debug/hop genpkg:c examples/imports-default/app`
- Explicit alias for non-identifier path tail:
  - App: `examples/imports-explicit/app`
  - Dependency: `examples/imports-explicit/lib/math-v2`
  - Check package: `./_build/macos-aarch64-debug/hop check examples/imports-explicit/app`

## Package Example

- App package: `examples/packages/app`
- Dependency package: `examples/packages/math`
- Package check: `./_build/macos-aarch64-debug/hop check examples/packages/app`
- Generate C header: `./_build/macos-aarch64-debug/hop genpkg:c examples/packages/app`
