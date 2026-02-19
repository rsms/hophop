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
- Casts (`as`): `examples/casts.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/casts.sl`
- `defer`: `examples/defer.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/defer.sl`

## Types

- Struct/union/enum: `examples/aggregates.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/aggregates.sl`
- Pointers and arrays: `examples/pointers_arrays.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/pointers_arrays.sl`
- Variable-size struct (dependent trailing field): `examples/vss.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/vss.sl`
- SLP-2 references/slices (`mut&`, `[T]`, `mut[T]`, slicing, implicit view conversions):
  `examples/slp2_refs_slices.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/slp2_refs_slices.sl`
- SLP-2 allocator `new(ma, ...)` forms: `examples/slp2_new_allocator.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/slp2_new_allocator.sl`
- Struct composition (embedded fields, promoted selectors): `examples/struct_composition.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/struct_composition.sl`
- Type functions (receiver sugar, `x.len()`, `ma.new(...)`): `examples/type_functions.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/type_functions.sl`
- Function groups (explicit overload groups): `examples/function_groups.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/function_groups.sl`

## Control Flow

- `if`, `for`, `switch`, `break`, `continue`: `examples/control_flow.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/control_flow.sl`

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
