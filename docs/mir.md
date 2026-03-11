# MIR in SL

This document describes the MIR (Mid-level Intermediate Representation) implementation in this repository (`src/mir.c`, `src/mir.h`, `src/mir_exec.c`), how it is used today, and where it is being extended.

## What MIR is

MIR is a small, internal, expression-level IR used by compile-time evaluation.

- Builder: `src/mir.c` builds MIR from AST expression nodes (`SLMirBuildExpr`).
- Program builder: `src/mir.c` now also exposes `SLMirProgramBuilder` helpers for assembling backend-facing MIR programs incrementally.
- IR shape: `src/mir.h` defines a compact stack-machine instruction format.
- Interpreter core: `src/mir_exec.c` executes MIR chunks.
- CTFE wrapper: `src/ctfe.c` builds expression MIR and delegates execution to `src/mir_exec.c`.

MIR is not yet the main lowering IR for runtime execution, but the repo now carries the beginning of a backend-facing MIR program model in `src/mir.h`. The extra function/program/metadata structs and runtime-oriented opcodes are scaffolding for that migration; current builders/executors still only use the expression subset.

## Current role in the compiler

Today, MIR is used for const-evaluating expressions.

- Typechecker const-eval path (`src/typecheck.c`) calls `SLCTFEEvalExpr`, which first builds MIR and then interprets it.
- C backend const folding (`src/codegen_c.c`) uses `SLConstEvalSession*` APIs; those APIs route through the same const-eval path and therefore use MIR for expression evaluation.
- The CLI evaluator path (`src/slc.c`) also calls `SLCTFEEvalExpr` for expression evaluation inside its execution engine.

MIR is expression-only. Statement/block/control-flow const execution is handled by `src/ctfe_exec.c`.

## IR model

MIR is a postfix (stack-based) instruction stream.

- Each instruction is `SLMirInst { op, tok, start, end }`.
- `start`/`end` are source byte offsets for diagnostics and literal decoding.
- `tok` is a small operand slot:
  - unary/binary ops: token kind (`SLTokenKind`)
  - `CALL`: argument count (`uint16_t`)
  - literal push ops: token discriminator when needed (for example, int vs rune)
- A compiled expression is `SLMirChunk { v, len }`, arena-allocated.

The builder emits children before parent operators, so the stream is directly executable by a stack machine.

## Opcode set (current)

From `SLMirOp` in `src/mir.h`:

- `SLMirOp_PUSH_INT`
- `SLMirOp_PUSH_FLOAT`
- `SLMirOp_PUSH_BOOL`
- `SLMirOp_PUSH_STRING`
- `SLMirOp_PUSH_NULL`
- `SLMirOp_LOAD_IDENT`
- `SLMirOp_CALL`
- `SLMirOp_UNARY`
- `SLMirOp_BINARY`
- `SLMirOp_INDEX` (element index; non-slice form)
- `SLMirOp_RETURN`

## AST coverage and limits

`SLMirBuildExprNode` currently supports:

- literals: `INT`, `RUNE`, `FLOAT`, `BOOL`, `STRING`, `NULL`
- identifiers: `IDENT`
- calls where callee is a plain identifier (`IDENT`)
- unary operators: `+`, `-`, `!`
- binary operators:
  - arithmetic: `+`, `-`, `*`, `/`, `%`
  - bitwise: `&`, `|`, `^`, `<<`, `>>`
  - comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
  - logical: `&&`, `||`
- indexing: `x[i]` element form (slice/range forms are unsupported in MIR and fall back)

Notable unsupported cases include:

- non-identifier call targets (for example field/method style callees)
- AST node kinds outside the list above
- operators outside allowed unary/binary token sets

Unsupported is a normal outcome, not always an error.

## Build contract (`SLMirBuildExpr`)

`SLMirBuildExpr(...)` has two success modes:

- returns `0` and `*outSupported = 1`: MIR chunk built, with trailing `RETURN`
- returns `0` and `*outSupported = 0`: expression not representable in current MIR subset

It returns `-1` only for hard errors (invalid API input, malformed critical AST shape, OOM), with `diag` set when available.

Important behavior:

- Memory is arena-owned. `outChunk->v` is valid while the arena is alive.
- Unsupported paths intentionally avoid hard failure so callers can fall back.

## Execution contract (`SLCTFEEvalExpr`)

`SLCTFEEvalExpr(...)`:

1. Builds MIR via `SLMirBuildExpr`.
2. If unsupported, returns `0` with `*outIsConst = 0`.
3. Otherwise delegates to `SLMirEvalChunk(...)` in `src/mir_exec.c`.

Interpreter details:

- Literal values are decoded from source slices (`start`/`end`) at execution time.
- `LOAD_IDENT` is resolved by callback (`SLCTFEResolveIdentFn`).
- `CALL` is resolved by callback (`SLCTFEResolveCallFn`) with popped arguments in source order.
- `RETURN` expects exactly one stack value; then sets `*outIsConst = 1`.

Return behavior:

- `0` + `*outIsConst = 1`: const value produced.
- `0` + `*outIsConst = 0`: not const-evaluable in current context/subset.
- `-1`: hard failure (for example OOM or decoding diagnostic errors).

## Interaction with consteval and `ctfe_exec`

Const-eval in the typechecker is layered:

- `SLTCEvalConstExprNode` handles special expression forms directly (`sizeof`, `cast`).
- For the remaining expression subset, it calls `SLCTFEEvalExpr` (MIR path).
- For const function bodies/statements, `SLTCResolveConstCall` uses `SLCTFEExecEvalBlock` (`src/ctfe_exec.c`).
- `ctfe_exec` delegates expression evaluation back through callbacks to `SLTCEvalConstExprNode`, which usually routes to MIR.

So today:

- MIR is the expression evaluator backend.
- `ctfe_exec` is still the statement/control-flow evaluator backend.
- `mir_exec` is now the dedicated MIR execution module that future runtime MIR work should extend instead of growing `ctfe.c`.

## Notes from `consteval` branch

This document was written after also checking the current `consteval` worktree branch.

At inspection time:

- `src/mir.c` and `src/mir.h` were unchanged relative to this branch.
- Const-eval value space in `ctfe` was being expanded (for example type-valued const results and reflection helpers in typecheck/ctfe paths).
- Reflection builtins are currently handled in typechecker const-eval logic before/around MIR usage.

Implication: MIR is still expression-stack IR, but ongoing consteval work may require expanding value handling and possibly MIR shape in later steps.

## Possible future use

MIR could evolve from a const-expression helper IR into a backend-facing IR layer.

Current architectural direction:

- keep MIR stack-based
- add a full `SLMirProgram` model with functions, const pool, field/type metadata, and symbol tables
- move runtime evaluator execution onto MIR instead of AST/callback execution
- use the same MIR executor substrate for consteval and runtime modes
- keep host-backed operations explicit instead of burying them in evaluator AST special cases

Likely requirements for that evolution:

- richer typed operand model
- explicit constant payloads (instead of source-slice-only decoding)
- stable control-flow representation
- clear debug/source mapping policy for non-CTFE consumers

## If you modify MIR

Minimum checklist:

1. Update opcode/type definitions in `src/mir.h`.
2. Update builder logic in `src/mir.c`.
3. Update interpreter behavior in `src/ctfe.c`.
4. Update const-eval integration paths in `src/typecheck.c` and any caller-specific resolvers.
5. Add tests that cover both:
   - supported paths (`outSupported = 1`, `outIsConst = 1`)
   - fallback paths (`outSupported = 0` or `outIsConst = 0`) without hard errors.
