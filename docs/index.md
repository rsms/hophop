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
_build/macos-aarch64-debug/slc compile <dir|file.sl> -o <exe>
_build/macos-aarch64-debug/slc run <dir|file.sl>
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
- `lib/sl-prelude.h`
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

## Feature Proposals (SLP)

- `docs/SLP-1-variable-size-structs.md`
- `docs/SLP-2-types.md`
- `docs/SLP-3-optional.md`
- `docs/SLP-4-type-functions.md`
- `docs/SLP-5-platform.md`
- `docs/SLP-6-struct-composition.md`
