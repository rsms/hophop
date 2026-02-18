# SL language project

## Build and Test

- `./build.sh test` — build (debug) and run full test suite
- `./build.sh` — build debug into `_build/macos-aarch64-debug/`
- `./build.sh release` — build release into `_build/macos-aarch64-release/`
- `./build.sh verbose=1` — show compiler commands
- Run the CLI: `_build/macos-aarch64-debug/slc`

The build script invokes `clang-format` automatically, so source files you edit will be reformatted on build.

Tests live at the end of `build.sh`. Each test is a `.sl` file in `tests/` paired with optional companion files (`.tokens`, `.stderr`, `.ast`) that define expected output. Add tests there when adding new functionality.

## CLI Commands

```sh
slc tokens file.sl          # tokenize
slc ast file.sl             # parse + print AST
slc check file.sl           # typecheck single file
slc checkpkg <dir|file.sl>  # typecheck package
slc genpkg:c <dir|file.sl> [out.h]  # generate C header
slc compile <dir|file.sl> -o <exe>  # compile via C11 backend + system compiler
slc run <dir|file.sl>       # compile + execute
```

## Architecture

SL is a language currently compiled via a C11 backend in a multi-stage pipeline:

```
Source → [Lexer] → Tokens → [Parser] → AST → [Typechecker] → [Codegen] → C header
```

### Source Layout (`src/`)

| File | Role |
|---|---|
| `libsl.h` | Public API: type definitions, token/AST node kinds, function declarations |
| `libsl.c` | Freestanding core: arena allocator, lexer (with semicolon insertion), diagnostics |
| `parse.c` | Recursive-descent parser; builds a flat array of AST nodes (indexed, not pointer-linked) |
| `typecheck.c` | Symbol tables, name resolution, type inference, mutability/reference checking |
| `slc.c` | Hosted CLI: package loading, import graph, command dispatch |
| `slc_codegen_c.c` | C backend: emits single-header library, mangles symbols, lowers defer, VSS accessors |
| `slc_codegen.h/.c` | Backend interface (currently only C backend exists) |
| `tools/amalgamate.py` | Merges `libsl.*` sources into a single distributable header |

### Key Design Points

**Freestanding core**: `libsl.h/c` and `parse.c`/`typecheck.c` have no libc dependencies. All allocation goes through an explicit `SLArena` arena passed by the caller. The hosted CLI (`slc.c`) provides the growable backing store.

**Flat AST**: Nodes are stored in a contiguous array; cross-references use `int32_t` indices, not pointers. This simplifies serialization and avoids pointer chasing.

**Single-header output**: `genpkg:c` emits a `.h` file (C11, compilable as freestanding) containing all type definitions, function declarations, and implementations for a package.

**Symbol mangling**: Package symbols are emitted as `pkg__Name` (double underscore separates package from identifier).

**Selector inference**: The typechecker resolves `.` vs `->` so the codegen always knows whether to emit `.` or `->` for field access.

**Defer lowering**: `defer` statements are reordered to LIFO at block exits during codegen, not in the AST.

**Variable-size structs (SLP-1)**: Structs can have a trailing `[.lenField]T` dependent array. Codegen emits accessor functions and runtime `sizeof` helpers. See `docs/SLP-1-variable-size-structs.md`.

**References and slices (SLP-2)**: `&T` = borrowed reference, `*T` = owned pointer, `[T]` = read-only slice, `mut[T]` = mutable slice. `new(ma, T)` allocates via an explicit `MemAllocator`. See `docs/SLP-2-types.md`.

## Workflow Notes

- Take an incremental approach: keep the compiler working at each step.
- After changes, run `./build.sh test` and update `build.sh` tests to cover new behavior.
- The language spec and EBNF grammar is in `docs/language.md`; project overview is in `docs/project-overview.md`; feature proposals are in `docs/SLP-*.md`.
