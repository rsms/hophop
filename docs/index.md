# SL

SL is a small, statically-checked language with a C11 backend.

The project has two parts:
- `libsl`: lexer, parser, typechecker, diagnostics, and codegen interfaces in C.
- `slc`: hosted CLI for checking packages, generating C, compiling, and running.

Generated output is designed to stay portable and straightforward to inspect.

## Start

Build and test:

```sh
./build.sh test
```

Common CLI flow:

```sh
_build/macos-aarch64-debug/slc checkpkg <dir|file.sl>
_build/macos-aarch64-debug/slc genpkg:c <dir|file.sl> [out.h]
_build/macos-aarch64-debug/slc [--cache-dir <dir>] compile <dir|file.sl> [-o <exe>]
_build/macos-aarch64-debug/slc [--cache-dir <dir>] run <dir|file.sl>
```

## Read By Goal

Learn the language:
- `docs/language.md` (syntax, semantics, grammar)
- `docs/library.md` (built-in types/functions and platform API)

Understand architecture and compiler behavior:
- `docs/project-overview.md`
- `README.md`

Work on runtime/platform integration:
- `docs/SLP-5-platform.md`
- `lib/platform_libc.c`

Work on compiler/library APIs:
- `src/libsl.h` (`SLLex`, `SLParse`, `SLTypeCheck`, diagnostics)
- `src/slc.c` (CLI flow)
- `src/codegen_c.c` (C backend)

## Current Language Surface

Implemented core includes:
- packages/imports/exports
- structs, unions, enums, functions, consts
- references, pointers, arrays, slices
- optional types (`?T`)
- variable-size structs
- typed contexts/capabilities (`context`, `with`)

## Feature Proposals (SLP)

- [SLP-1-variable-size-structs.md](SLP-1-variable-size-structs.md)
- [SLP-2-types.md](SLP-2-types.md)
- [SLP-3-optional.md](SLP-3-optional.md)
- [SLP-4-type-functions.md](SLP-4-type-functions.md)
- [SLP-5-platform.md](SLP-5-platform.md)
- [SLP-6-struct-composition.md](SLP-6-struct-composition.md)
- [SLP-7-imports.md](SLP-7-imports.md)
- [SLP-8-parameter-runs.md](SLP-8-parameter-runs.md)
- [SLP-9-enum-member-namespaces.md](SLP-9-enum-member-namespaces.md)
- [SLP-10-variable-type-inference.md](SLP-10-variable-type-inference.md)
- [SLP-11-function-types.md](SLP-11-function-types.md)
- [SLP-12-capabilities.md](SLP-12-capabilities.md)
- [SLP-13-compound-literals.md](SLP-13-compound-literals.md)
- [SLP-14-anonymous-struct-types.md](SLP-14-anonymous-struct-types.md)
- [SLP-15-struct-field-defaults.md](SLP-15-struct-field-defaults.md)
- [SLP-16-anonymous-enums.md](SLP-16-anonymous-enums.md)
- [SLP-17-platform-context-composition.md](SLP-17-platform-context-composition.md)
- [SLP-18-reflection.md](SLP-18-reflection.md)
- [SLP-19-pointer-slice-unification.md](SLP-19-pointer-slice-unification.md)
- [SLP-20-tagged-unions.md](SLP-20-tagged-unions.md)
- [SLP-21-variadic-function-parameters.md](SLP-21-variadic-function-parameters.md)
- [SLP-22-consteval-diagnostics.md](SLP-22-consteval-diagnostics.md)
- [SLP-23-const-semantics.md](SLP-23-const-semantics.md)
- [SLP-24-const-params.md](SLP-24-const-params.md)
- [SLP-25-anytype.md](SLP-25-anytype.md)
- [SLP-26-format-function.md](SLP-26-format-function.md)
- [SLP-27-const-numeric-literals.md](SLP-27-const-numeric-literals.md)
- [SLP-28-call-site-compile-time-validation.md](SLP-28-call-site-compile-time-validation.md)
- [SLP-29-for-in-loops.md](SLP-29-for-in-loops.md)
- [SLP-30-iterator-protocol.md](SLP-30-iterator-protocol.md)
- [SLP-31-nested-types.md](SLP-31-nested-types.md)
- [SLP-32-wasm-backend.md](SLP-32-wasm-backend.md)
