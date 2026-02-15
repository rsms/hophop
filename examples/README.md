# SL Examples

`fn main` is special and does not need to be listed in a `pub { ... }` block.

Single-file mode is supported for package commands:

- `./_build/macos-aarch64-debug/slc checkpkg examples/single-file/main.sl`
- `./_build/macos-aarch64-debug/slc genpkg:c examples/single-file/main.sl`

## Core

- Basic function: `examples/basic/main.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/basic/main.sl`
- Declaration-order independence: `examples/order_independent/main.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/order_independent/main.sl`
- Strings, `assert`, `len`, `cstr`: `examples/strings/main.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/strings/main.sl`
- Casts (`as`): `examples/casts/main.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/casts/main.sl`
- `defer`: `examples/defer/main.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/defer/main.sl`

## Types

- Struct/union/enum: `examples/aggregates/main.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/aggregates/main.sl`
- Pointers and arrays: `examples/pointers_arrays/main.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/pointers_arrays/main.sl`

## Control Flow

- `if`, `for`, `switch`, `break`, `continue`: `examples/control_flow/main.sl`
  - Check: `./_build/macos-aarch64-debug/slc check examples/control_flow/main.sl`

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
