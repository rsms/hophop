# SL Examples

`fn main` is special and does not need to be listed in a `pub { ... }` block.

## Basic single-file package

- Source: `examples/basic/main.sl`
- Typecheck: `./_build/macos-aarch64-debug/slc check examples/basic/main.sl`

## Strings + assert + prelude helpers

- Source: `examples/strings/main.sl`
- Typecheck: `./_build/macos-aarch64-debug/slc check examples/strings/main.sl`

## Multi-package import example

- App package: `examples/packages/app`
- Dependency package: `examples/packages/math`
- Package check: `./_build/macos-aarch64-debug/slc checkpkg examples/packages/app`
- Generate C header: `./_build/macos-aarch64-debug/slc genpkg:c examples/packages/app`
