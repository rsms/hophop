# Wasm target

This document is a developer-facing overview of the SL Wasm target.

It explains how the target is structured, how backend-facing MIR is lowered to
WebAssembly, and how the final module is assembled. It intentionally avoids
describing unstable implementation details that are likely to move over time.

## What the Wasm target is

The Wasm target is a direct compiler backend.

Its job is to take a checked SL package that has already been lowered to
backend-facing MIR and produce a deterministic `.wasm` module without going
through C or an external code generator.

Important separation:

- the **Wasm target** is the backend itself
- a **Wasm platform** is an execution environment or ABI layered on top

That means the backend is responsible for generic Wasm code generation, while
platform concerns such as host imports, startup behavior, or smoke-test runtime
support are handled separately.

## Place in the compiler

The Wasm backend sits after parsing, type checking, and MIR formation.

The high-level flow is:

```text
SL source
  -> parse
  -> typecheck
  -> package-level MIR lowering and rewrites
  -> backend-facing MIR program
  -> Wasm lowering
  -> binary Wasm module
```

The key design choice is that Wasm codegen consumes backend-facing MIR, not raw
AST and not evaluator-style dynamic resolution.

By the time the Wasm backend runs, the program should already have:

- explicit functions, locals, constants, fields, and type metadata
- resolved direct calls and other backend-relevant references
- a control-flow shape that can be mapped to structured Wasm constructs

If the MIR still depends on generic late resolution, codegen should fail rather
than silently guessing.

## Lowering model

The backend lowers one MIR program into one Wasm module.

At a high level, lowering has four jobs:

1. map MIR value shapes to Wasm value or memory shapes
2. map MIR control flow to structured Wasm control flow
3. assign deterministic linear-memory layout where values cannot stay purely in
   Wasm locals
4. encode the finished functions, globals, data, imports, exports, and other
   sections into a binary module

### Values and storage

The backend uses a mixed model:

- simple scalar and pointer-like values stay in Wasm locals / Wasm stack values
- composite values lower through linear memory

This keeps the direct Wasm path simple while still supporting language features
that naturally need addresses, aggregate layout, or caller-owned storage.

Conceptually, the backend has to classify each MIR value into one of these
styles:

- plain Wasm value
- multiple Wasm values that together represent one SL value
- memory-backed value addressed through linear memory

That classification then drives local handling, call lowering, return lowering,
and memory layout.

### Control flow

MIR control flow is more general than Wasm’s structured control flow, so the
backend has to recognize reducible regions and emit them as structured Wasm.

In practice this means:

- straight-line regions lower directly
- conditionals lower to `if` / `else`
- reducible loops lower to `block` + `loop`
- breaks and continues become depth-based branches within that structured shape

The backend does not treat arbitrary jump graphs as acceptable. Unsupported or
irreducible shapes fail explicitly instead of producing incorrect Wasm.

### Calls and returns

Direct same-program calls are lowered from MIR call edges to Wasm calls using an
internal calling convention derived from the MIR-visible value shapes.

Where SL values do not fit naturally into a plain Wasm return, the backend uses
internal memory-based conventions, such as caller-owned result storage. This is
an internal backend ABI decision, not a stable public interface.

Indirect calls use the same overall rule: they are lowered from typed MIR
function values to Wasm table-based calls, with signature matching based on the
backend’s Wasm-level function shape.

## Linear memory and layout

Wasm gives the backend one linear memory, so all addressable data has to live in
that space.

The backend uses deterministic layout rules for:

- static data
- function-local frame-backed storage
- heap-backed storage for dynamic allocation paths
- reserved runtime regions needed by entry wrappers or platform support

The important architectural point is that these are all backend-managed regions
within one memory, not separate heaps or separate address spaces.

This lets the backend use one consistent addressing model for:

- references and pointers
- aggregate field access
- array and slice indexing
- memory-backed call and return conventions
- platform-facing string or slice payloads

The exact byte layout is an implementation detail, but the rules must remain
deterministic so the same input package always produces the same module shape.

## How the module is built

The backend writes the binary Wasm module directly.

The final module is assembled from the information discovered during lowering:

- function signatures
- imported functions required by the selected platform layer
- whether a function table is needed
- whether globals are needed for frame or heap state
- static data blobs
- entrypoint wrapper needs

At a high level, module construction works like this:

1. analyze the MIR program and decide which Wasm resources are needed
2. compute function signatures and any shared signature table entries
3. compute import and export layout
4. compute static-data placement and any reserved memory regions
5. emit Wasm sections in binary form
6. emit function bodies after their signatures and indices are fixed

The writer is intentionally simple: it encodes the Wasm binary format directly
instead of going through a text format or a third-party assembler.

## Target vs platform

The backend is not the same thing as a platform runtime.

A useful way to think about it:

- the **target** answers: “how does this MIR program become valid Wasm?”
- the **platform** answers: “what imports, entry behavior, and runtime services
  does this Wasm module expect?”

This separation matters because the same backend can be paired with different
platform policies later. The backend owns generic Wasm lowering; the platform
layer owns environment-specific host behavior.

The existing smoke-test platform is one instance of that layering, not the
definition of the Wasm target itself.

## Development and debugging

When working on the Wasm backend, the main debugging surface is the backend-facing
MIR dump, not the final Wasm bytes.

Typical workflow:

1. inspect MIR for the test case or package
2. confirm that the MIR is fully lowered and deterministic enough for Wasm
3. reason about how that MIR should map to Wasm values, memory, and control flow
4. only then inspect Wasm structure if needed

This keeps backend work grounded in the actual lowering boundary instead of
mixing language-level issues with binary-encoding issues too early.

## What this document is not

This document is an architectural overview.

It is not:

- a stable public Wasm ABI reference
- a platform ABI document
- a full catalog of supported language features
- a line-by-line explanation of the current implementation

For feature scope and bring-up history, see `docs/SLP-32-wasm-backend.md`.
For MIR structure and semantics, see `docs/mir.md`.
