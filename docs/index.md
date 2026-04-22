# HopHop

HopHop is a small, statically-checked language with a C11 backend.

The project has two parts:
- `libhop`: lexer, parser, typechecker, diagnostics, and codegen interfaces in C.
- `hop`: hosted CLI for checking packages, generating C, compiling, and running.

Generated output is designed to stay portable and straightforward to inspect.

## Start

Build and test:

```sh
./build.sh test
```

Common CLI flow:

```sh
_build/macos-aarch64-debug/hop checkpkg <dir|file.hop>
_build/macos-aarch64-debug/hop genpkg:c <dir|file.hop> [out.h]
_build/macos-aarch64-debug/hop compile [--cache-dir <dir>] <dir|file.hop> [-o <exe>]
_build/macos-aarch64-debug/hop run [--cache-dir <dir>] <dir|file.hop>
```

`hop run` defaults to `--platform cli-eval`. Other platform-aware commands still default to
`cli-libc` when `--platform` is omitted.

## Read By Goal

Learn the language:
- `docs/language.md` (syntax, semantics, grammar)
- `docs/library.md` (built-in types/functions and platform API)

Understand architecture and compiler behavior:
- `docs/project-overview.md`
- `docs/wasm.md`
- `README.md`

Work on runtime/platform integration:
- `docs/HEP-5-platform.md`
- `lib/platform_libc.c`

Work on compiler/library APIs:
- `src/libhop.h` (`HOPLex`, `HOPParse`, `HOPTypeCheck`, diagnostics)
- `src/hop.c` (CLI flow)
- `src/codegen_c.c` (C backend)

## Current Language Surface

Implemented core includes:
- packages/imports/exports
- structs, unions, enums, functions, consts
- references, pointers, arrays, slices
- optional types (`?T`)
- variable-size structs
- typed contexts/capabilities (`context`)

## Feature Proposals (HEP)

- [HEP-1-variable-size-structs.md](HEP-1-variable-size-structs.md)
- [HEP-2-types.md](HEP-2-types.md)
- [HEP-3-optional.md](HEP-3-optional.md)
- [HEP-4-type-functions.md](HEP-4-type-functions.md)
- [HEP-5-platform.md](HEP-5-platform.md)
- [HEP-6-struct-composition.md](HEP-6-struct-composition.md)
- [HEP-7-imports.md](HEP-7-imports.md)
- [HEP-8-parameter-runs.md](HEP-8-parameter-runs.md)
- [HEP-9-enum-member-namespaces.md](HEP-9-enum-member-namespaces.md)
- [HEP-10-variable-type-inference.md](HEP-10-variable-type-inference.md)
- [HEP-11-function-types.md](HEP-11-function-types.md)
- [HEP-12-capabilities.md](HEP-12-capabilities.md)
- [HEP-13-compound-literals.md](HEP-13-compound-literals.md)
- [HEP-14-anonymous-struct-types.md](HEP-14-anonymous-struct-types.md)
- [HEP-15-struct-field-defaults.md](HEP-15-struct-field-defaults.md)
- [HEP-16-anonymous-enums.md](HEP-16-anonymous-enums.md)
- [HEP-17-platform-context-composition.md](HEP-17-platform-context-composition.md)
- [HEP-18-reflection.md](HEP-18-reflection.md)
- [HEP-19-pointer-slice-unification.md](HEP-19-pointer-slice-unification.md)
- [HEP-20-tagged-unions.md](HEP-20-tagged-unions.md)
- [HEP-21-variadic-function-parameters.md](HEP-21-variadic-function-parameters.md)
- [HEP-22-consteval-diagnostics.md](HEP-22-consteval-diagnostics.md)
- [HEP-23-const-semantics.md](HEP-23-const-semantics.md)
- [HEP-24-const-params.md](HEP-24-const-params.md)
- [HEP-25-anytype.md](HEP-25-anytype.md)
- [HEP-26-format-function.md](HEP-26-format-function.md)
- [HEP-27-const-numeric-literals.md](HEP-27-const-numeric-literals.md)
- [HEP-28-call-site-compile-time-validation.md](HEP-28-call-site-compile-time-validation.md)
- [HEP-29-for-in-loops.md](HEP-29-for-in-loops.md)
- [HEP-30-iterator-protocol.md](HEP-30-iterator-protocol.md)
- [HEP-31-nested-types.md](HEP-31-nested-types.md)
- [HEP-32-wasm-backend.md](HEP-32-wasm-backend.md)
- [HEP-33-rawptr.md](HEP-33-rawptr.md)
- [HEP-34-link-time-imports.md](HEP-34-link-time-imports.md)
