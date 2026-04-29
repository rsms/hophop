# HEP-32 MIR-to-Wasm backend

## Summary

HEP-32 adds a new `wasm` compiler backend that lowers checked HopHop packages through
backend-facing MIR (`HOPMirProgram`) to a WebAssembly module.

The first milestone is intentionally narrow:

- target Wasm directly, not C
- keep the backend platform-neutral
- do not target WASI, Playbit, JS host APIs, or the component model
- allow an optional minimal bring-up platform for smoke tests and early execution

This is a compiler/backend proposal. It does not add or change language syntax.

## Motivation

Today HopHop has one production codegen backend: C11.

That works well for portability, but it has limits:

- it requires a host C toolchain after `hop`
- it makes Wasm output indirect
- it couples non-C targets to C ABI and C codegen choices

The repository already has a growing backend-facing MIR model in `docs/mir.md`.
That makes it reasonable to add a direct backend that consumes MIR instead of
starting from AST or piggybacking on C.

The goal is to make Wasm a real compiler target while keeping platform policy
separate from backend mechanics.

## Goals

- Add a `wasm` backend to `hop`.
- Lower from checked package state through `HOPMirProgram`, not through C.
- Produce deterministic Wasm modules without a required external codegen toolchain.
- Keep the initial backend independent from any specific Wasm platform ABI.
- Permit incremental implementation by accepting a conservative MIR/runtime subset first.
- Keep failure explicit: unsupported MIR or unsupported runtime features must report
  a direct Wasm-backend diagnostic instead of silently falling back.

## Non-goals

- Defining a stable WASI target.
- Defining a stable Playbit target.
- Defining a stable JS embedding ABI.
- Replacing the C backend.
- Full language/runtime coverage in the first implementation.
- Optimizing for peak Wasm performance in v1.
- Multi-module linking, dynamic linking, threads, exceptions, SIMD, GC, or component-model support.

## Status

Completed for the intended `wasm-min` bootstrap scope.

This HEP defined the backend shape and bring-up plan. The active manifest now covers
that intended bootstrap surface; remaining Wasm work is follow-up outside HEP-32.

Current implementation status: the backend can now lower the bring-up subset through
`wasm-min`, including pure scalar control flow, `platform.exit`, `platform.console_log`,
assert panic reporting, and `&str` values carried as Wasm `(ptr,len)` pairs through
locals and direct calls. The current memory subset also includes typed integer dereference,
string byte indexing, local fixed-array element addressing, frame-backed local aggregates,
frame-backed fixed-array local copy, aggregate field address/load/store, scalar-field aggregate
compound literals and typed local assignment, nested aggregate field initialization by fixed-size
payload copy, same-program aggregate and fixed-array helper returns through an internal hidden
out-pointer ABI, fixed-size zero-init `alloc` for scalar pointers, aggregate pointers, and fixed-array
pointers through an internal Wasm heap cursor, dynamic-count zero-init `alloc [T n]` for the current
typed integer slice-pointer subset plus fixed-layout aggregate slice elements, and slice/view
creation over the current fixed-array and pointer-backed linear-memory bring-up path. `wasm-min`
now also supports explicit allocator-form `alloc` for root/null allocator bring-up shapes
(`context.mem`, `temp_mem`, and local aliases that flow through those values) with null optional
results and unwrap-on-null panic for non-optional results. Narrow custom allocator impl dispatch
also works for allocator values layered on `context.mem` through top-level `Allocator.impl`
functions and the allocator callback signature. Fixed-size aggregate heap init now works for direct
field initializers and non-VSS default-init paths (`alloc T{...}`, `alloc T{}`, and plain `alloc T`
where omitted direct fields use defaults). VSS-backed heap init and `str`-object heap init now
work for the current direct-field subset under `wasm-min`, including inline `name.len`
initialization, zeroed VSS tails, and plain `alloc str{ len: n }`. Top-level function values now
also work through Wasm tables for the current supported subset, including local aliases,
function-typed params and returns, and aggregate field calls that lower to `CALL_INDIRECT`,
while the existing allocator callback path continues to use the same table machinery. Exported
`main` now supports the current non-scalar
Wasm subset through an internal `hop_main` wrapper and reserved result buffer for aggregate,
fixed-array, `&str`, slice/view, and one-slot pointer-like returns. Reducible unconditional loops
from `for { ... }` now lower through the same structured Wasm loop path as the existing
condition-headed loop subset. The emitter also fails fast on pathological Wasm body growth and
no-progress range emission instead of spending unbounded time in repeated buffer growth or
recursive range traversal. The active test manifest now covers the intended `wasm-min` bootstrap
surface for HEP-32. Non-top-level or capturing function values remain post-HEP follow-up work.

## Design overview

Proposed pipeline:

```text
HopHop source
  -> parse + typecheck
  -> checked package lowering
  -> HOPMirProgram
  -> optional lower CFG/SSA IR
  -> Wasm lowering + stackification
  -> .wasm module
```

Bring-up priority:

1. emit correct Wasm for a small MIR subset
2. expand language/runtime coverage
3. add lower-IR optimization for output quality

Correctness comes before optimization in the first implementation.

Important constraint:

- the Wasm backend consumes fully lowered backend-facing MIR
- generic evaluator-only dynamic resolution is not allowed at Wasm codegen time

Concretely, a package is Wasm-codegenable only when the resulting MIR program no longer
depends on runtime callbacks such as generic `LOAD_IDENT` or generic `CALL`.
If `HOPMirProgramNeedsDynamicResolution(...)` is true after lowering/rewrite, Wasm codegen
must fail with a dedicated diagnostic.

## CLI and artifact shape

The backend is exposed as:

```sh
hop build --platform wasm-min <dir|file.hop> -o out.wasm
```

Notes:

- `build --platform wasm-min` emits a Wasm binary module.
- A debug-only text form such as WAT may be added later, but is not required by this HEP.

Because the current codegen interface is C-oriented (`emit` returns one text buffer),
implementing `wasm` will require extending that interface so a backend can emit binary
artifacts with an explicit byte length.

This interface change is part of the backend work, not a separate language feature.

## Implementation strategy

The backend should be implemented in C inside `libhop`, without requiring libc, libc++, or a host
Wasm toolchain at runtime.

Recommended structure:

- a small handwritten Wasm binary writer
- a Wasm-specific lowering pass from `HOPMirProgram`
- a later optional lower CFG/SSA IR for optimization before final Wasm emission
- use `hop build --output-format mir <package-dir|file.hop>` as the primary
  development/debugging surface for backend-facing MIR

Rationale:

- the Wasm binary format is regular enough that writing modules directly is practical
- a handwritten emitter keeps `libhop` freestanding and small
- the difficult parts are lowering, ABI, structured control flow, data layout, and optimization,
  not byte emission itself

This HEP treats the Wasm encoder as implementation detail. The important requirement is that the
implementation remain compatible with the freestanding `libhop` constraints.

## MIR inspection and debugging

The repository now has a MIR inspection command:

```sh
hop build --output-format mir <package-dir|file.hop>
```

This prints the current backend-facing `HOPMirProgram` in a deterministic text format.

For Wasm backend work, this should be the default debugging surface before looking at emitted Wasm.

What it is useful for:

- confirming which package-level functions and top-level init functions are present
- checking whether MIR still needs dynamic resolution
- inspecting locals, const tables, host refs, symbol refs, type refs, and field refs
- checking control-flow lowering and jump targets
- catching unsupported MIR shapes before blaming Wasm emission

Development guidance:

- when adding or debugging Wasm lowering, first inspect `build --output-format mir` output for the
  same fixture
- when adding alloc MIR rewrites for Wasm readiness, prefer adding or updating MIR golden tests
- use Wasm-level golden tests only after the MIR shape is already understood and stable enough

## Wasm target model

The initial backend targets portable core Wasm with a conservative feature set:

- Wasm MVP integer and float instructions
- one linear memory
- `memory.size` / `memory.grow`
- mutable globals as needed
- structured control flow (`block`, `loop`, `if`, `br`, `br_if`)

The initial target does not require:

- threads
- exceptions
- reference types
- GC types
- SIMD
- multiple memories

The memory model is `wasm32`:

- HopHop pointers and references lower to 32-bit linear-memory addresses
- byte order is little-endian
- linear-memory layout is backend-defined but must be deterministic

## Value and ABI strategy

The first Wasm backend should use a conservative internal ABI.

Recommended v1 rule:

- scalar values (`bool`, integer scalars, `f32`, `f64`, pointer-like values) lower to Wasm numeric values
- non-scalar aggregates lower through linear-memory addresses
- complex returns may use an out-pointer convention or return an address to caller-owned storage

This ABI is backend-internal in v1. It does not need to be stable for external consumers yet.

Rationale:

- it keeps MIR lowering simple
- it avoids forcing multi-value or platform-specific ABI choices early
- it matches the current goal of "make Wasm backend work first"

## MIR requirements

The backend targets `HOPMirProgram`, not raw AST.

For v1, the Wasm backend may require all of the following:

- validated `HOPMirProgram`
- resolved function graph
- explicit locals and types
- resolved field/type/host metadata tables
- no remaining generic `LOAD_IDENT`
- no remaining generic `CALL`

Expected supported MIR early in bring-up:

- constants and scalar arithmetic
- local loads/stores
- direct same-program calls via `CALL_FN`
- direct indirect calls where MIR already materializes function refs
- simple control flow (`JUMP`, `JUMP_IF_FALSE`, `RETURN`, `RETURN_VOID`)
- explicit memory-oriented ops needed for locals and references

Expected unsupported areas at first:

- any MIR path that still depends on evaluator-private hooks
- hostcalls outside the bootstrap set
- backend features that need aggregate lowering not yet represented in MIR

Unsupported MIR must fail fast with a clear Wasm-backend diagnostic.

`hop build --output-format mir` is the intended way to inspect whether those requirements are
actually met for a given fixture or package during backend bring-up.

## Lower IR and optimization

Getting correct Wasm is only the first step. Producing decent-quality Wasm will likely require a
lower IR that is easier to optimize than the current stack-oriented MIR.

This HEP does not require one specific lower IR design, but it explicitly allows:

- direct MIR-to-Wasm lowering for early bring-up
- introducing a lower CFG-oriented IR later
- evolving that lower IR toward SSA if that proves useful

Recommended direction:

- start with direct MIR-to-Wasm lowering for the conservative subset
- once correctness and coverage exist, introduce a lower non-Wasm-specific IR for:
  - control-flow normalization
  - value lifetime tracking
  - dead code elimination
  - constant folding and propagation
  - simple load/store cleanup
  - call and local-slot simplification
- perform final Wasm stackification only near emission

This keeps optimization concerns separate from the first backend bring-up while leaving room for a
better long-term pipeline.

## Data layout

The backend owns concrete layout for:

- globals
- stack or frame spill space
- string literal storage
- aggregate storage
- optional hidden temporaries for calls and returns

Recommended v1 approach:

- emit string literals and other immutable blobs as data segments
- use one mutable global as a linear-memory heap cursor for simple allocation needs
- keep layout simple and deterministic before optimizing

This does not define the final runtime allocator strategy. It is only enough to bring the
backend up without committing to a platform ABI.

## Host boundary and platform separation

This HEP intentionally separates two concerns:

1. Wasm backend:
   turns MIR into Wasm instructions and module sections
2. Wasm platform:
   decides which imports/exports are used to talk to a host environment

The backend itself must not assume WASI or any other named Wasm platform.

### Pure backend mode

A package that does not need host services should be able to compile to a Wasm module with no
platform-specific imports.

### Bootstrap platform mode

For early bring-up and tests, the implementation may include one repository-owned minimal platform
target, tentatively `wasm-min`.

`wasm-min` is explicitly not a stable public target. It exists only to make backend bring-up,
smoke tests, and simple demos practical.

Permitted scope for `wasm-min`:

- only the minimum imports/exports needed to execute simple programs
- only the minimum `import "platform"` surface needed for those programs
- no claim of compatibility with WASI, browsers, JS, or Playbit

Reasonable `wasm-min` choices include:

- exporting `memory`
- exporting `hop_main` or another deterministic entry wrapper
- importing only a tiny host surface such as `exit(status)` or panic/log helpers

Current bring-up scope in this repository:

- pure modules with no imports
- one minimal host import, `platform.exit(i32)`, for smoke-test execution
- one minimal logging import, `platform.console_log(&str, i32)`, for bootstrap diagnostics
- one internal panic-reporting import used by Wasm `assert` lowering in `wasm-min`

If allocation is needed before a real platform exists, prefer an in-module simple allocator based
on linear memory growth over importing a large allocator ABI from the host.

## Interaction with `import "platform"`

`import "platform"` remains a language-level pseudo package as defined by HEP-5 and HEP-17.

This HEP does not define a new stable Wasm `platform` package.

Instead:

- packages that do not import `platform` should compile without a platform target
- packages that do import `platform` may require `--platform wasm-min` during bring-up
- a future HEP can define stable Wasm platform packages after backend experience exists

This keeps the initial backend from freezing the wrong host ABI too early.

## Export naming

Export naming must be deterministic.

For v1:

- internal Wasm function names are backend-defined
- exported entry names are backend-defined
- if a package `main` exists, the backend should expose one deterministic entry export for it

The exact public/exported naming scheme can remain backend-defined in v1, but it must be stable
within a compiler revision and documented by the implementation.

## Diagnostics

Recommended diagnostics:

- `wasm_backend_unsupported_mir`: MIR program still contains unsupported instructions or unresolved dynamic operations
- `wasm_backend_unsupported_hostcall`: hostcall is not part of the supported Wasm bootstrap surface
- `wasm_backend_platform_required`: package uses `import "platform"` features that need an explicit Wasm platform target
- `wasm_backend_internal`: internal Wasm codegen failure

## External libraries and tools

This backend should not require a third-party Wasm compiler library as part of `libhop`.

In particular, a C API alone is not sufficient reason to take a dependency if the actual library
implementation pulls in C++, libc++, or a larger hosted runtime than this project wants.

Guidance:

- do not require Binaryen, WABT, or another external Wasm library to build or run `libhop`
- keep the production emitter and lowering pipeline self-contained in C
- external tools are acceptable for development-time validation, debugging, comparison, or tests

Examples of acceptable optional use:

- validate emitted modules with external tools in developer workflows
- compare emitted WAT or binary structure during bring-up
- experiment with post-pass optimization outside the core compiler
- compare emitted Wasm against `hop build --output-format mir` output while debugging lowering
  mismatches

Examples of non-goals for v1:

- linking Binaryen into `libhop`
- depending on libc++ in the core compiler path
- making the Wasm backend require a hosted environment

## Implementation plan

### Phase 1: artifact boundary

- extend `HOPCodegenBackend` output handling so a backend can emit binary bytes, not only C text
- register backend name `wasm`
- add CLI support for Wasm output through `hop build`

### Phase 2: MIR package formation for Wasm

- add or extend package-level MIR lowering so codegen gets one validated `HOPMirProgram`
- ensure Wasm-targeted lowering can reject unresolved dynamic operations before emission
- keep `build --output-format mir` output useful and stable enough to debug that package-level MIR
  formation

### Phase 3: Wasm emitter

- implement a small Wasm binary module writer in C
- lower constants, functions, locals, control flow, and direct calls
- emit linear memory, globals, and data segments as needed
- keep the initial emitter simple and deterministic rather than optimized

### Phase 4: correctness and coverage

- expand the directly supported MIR/runtime subset
- make unsupported cases fail with precise Wasm-backend diagnostics
- keep platform-neutral pure-code paths working without host imports
- add MIR-dump regression coverage for newly supported lowering shapes where helpful
- current status includes `LOCAL_ADDR`, `DEREF_LOAD`, `DEREF_STORE`, `SEQ_LEN`, `STR_CSTR`,
  and byte indexing over `cstr(...)` results for the current linear-memory model

### Phase 5: bootstrap execution path

- optionally add `lib/platform/wasm-min/`
- support the minimum host boundary needed for smoke tests
- keep this platform clearly marked unstable and bring-up-only

### Phase 6: lower IR and optimization

- introduce an optimization-friendly lower IR if direct MIR-to-Wasm lowering stops producing
  acceptable output quality
- add simple optimization passes before final Wasm stackification
- improve code size and execution quality without tying the optimizer to a specific Wasm platform

### Phase 7: target-specific follow-up

- only after real usage, evaluate stable Wasm platform targets such as WASI or Playbit

## Test plan

Add tests in layers:

1. Backend formation:
   - `hop build --platform wasm-min` succeeds for simple pure functions and `main`
   - unresolved dynamic MIR fails with the expected diagnostic
   - corresponding MIR output clearly shows the unresolved operations that block Wasm codegen
2. Structural output:
   - emitted module contains expected sections, exports, and deterministic data layout
   - MIR goldens exist for representative backend-facing MIR programs used by Wasm tests
3. Execution smoke tests:
   - optional `wasm-min` runner can execute tiny programs and observe exit status or exported results
4. Quality regression tests:
   - golden WAT or structural expectations for control-flow lowering
    - code-size-sensitive fixtures for simple optimization wins once a lower IR exists
5. Negative coverage:
   - unsupported hostcalls
   - unsupported MIR ops
   - platform-required programs without a Wasm platform target

Prefer text or structured golden outputs for tests where possible. Raw binary goldens should be
used sparingly.

## Future work

- stable Wasm platform targets (`wasi`, Playbit, browser/JS host, etc.)
- better ABI rules for cross-module interop
- debug metadata
- optimization passes over a lower IR or SSA form before Wasm emission
- direct Wasm execution support in `hop run`
- richer allocator/runtime strategy once platform requirements are clearer
