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
- `docs/HEP-005-platform.md`
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

- [HEP-001-variable-size-structs.md](HEP-001-variable-size-structs.md)
- [HEP-002-types.md](HEP-002-types.md)
- [HEP-003-optional.md](HEP-003-optional.md)
- [HEP-004-type-functions.md](HEP-004-type-functions.md)
- [HEP-005-platform.md](HEP-005-platform.md)
- [HEP-006-struct-composition.md](HEP-006-struct-composition.md)
- [HEP-007-imports.md](HEP-007-imports.md)
- [HEP-008-parameter-runs.md](HEP-008-parameter-runs.md)
- [HEP-009-enum-member-namespaces.md](HEP-009-enum-member-namespaces.md)
- [HEP-010-variable-type-inference.md](HEP-010-variable-type-inference.md)
- [HEP-011-function-types.md](HEP-011-function-types.md)
- [HEP-012-capabilities.md](HEP-012-capabilities.md)
- [HEP-013-compound-literals.md](HEP-013-compound-literals.md)
- [HEP-014-anonymous-struct-types.md](HEP-014-anonymous-struct-types.md)
- [HEP-015-struct-field-defaults.md](HEP-015-struct-field-defaults.md)
- [HEP-016-anonymous-enums.md](HEP-016-anonymous-enums.md)
- [HEP-017-platform-context-composition.md](HEP-017-platform-context-composition.md)
- [HEP-018-reflection.md](HEP-018-reflection.md)
- [HEP-019-pointer-slice-unification.md](HEP-019-pointer-slice-unification.md)
- [HEP-020-tagged-unions.md](HEP-020-tagged-unions.md)
- [HEP-021-variadic-function-parameters.md](HEP-021-variadic-function-parameters.md)
- [HEP-022-consteval-diagnostics.md](HEP-022-consteval-diagnostics.md)
- [HEP-023-const-semantics.md](HEP-023-const-semantics.md)
- [HEP-024-const-params.md](HEP-024-const-params.md)
- [HEP-025-anytype.md](HEP-025-anytype.md)
- [HEP-026-format-function.md](HEP-026-format-function.md)
- [HEP-027-const-numeric-literals.md](HEP-027-const-numeric-literals.md)
- [HEP-028-call-site-compile-time-validation.md](HEP-028-call-site-compile-time-validation.md)
- [HEP-029-for-in-loops.md](HEP-029-for-in-loops.md)
- [HEP-030-iterator-protocol.md](HEP-030-iterator-protocol.md)
- [HEP-031-nested-types.md](HEP-031-nested-types.md)
- [HEP-032-wasm-backend.md](HEP-032-wasm-backend.md)
- [HEP-033-rawptr.md](HEP-033-rawptr.md)
- [HEP-034-link-time-imports.md](HEP-034-link-time-imports.md)
