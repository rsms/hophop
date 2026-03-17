# MIR in SL

This document describes the MIR (Mid-level Intermediate Representation) implementation in this repository (`src/mir.c`, `src/mir.h`, `src/mir_lower.c`, `src/mir_lower_pkg.c`, `src/mir_lower_stmt.c`, `src/mir_exec.c`), how it is used today, and where it is being extended.

## What MIR is

MIR is a small, internal, expression-level IR used by compile-time evaluation.

- Builder: `src/mir.c` builds MIR from AST expression nodes (`SLMirBuildExpr`).
- Lowering wrapper: `src/mir_lower.c` currently lowers one expression into a one-function `SLMirProgram`.
- Package/top-init lowering: `src/mir_lower_pkg.c` owns the current top-level initializer and zero-init lowering path.
- `mir_lower_pkg` now also exposes append-style and root-program top-init helpers, so package initializers can be assembled into one MIR program instead of only one-function wrappers or evaluator-side one-off setup.
- `mir_lower_pkg` now also owns AST var/const top-init shape extraction by declarator name slice, so evaluator-side top-const and top-var MIR lowering no longer has to pre-decode init/type nodes before calling into the package-lowering boundary.
- The consteval-side top-level const MIR path now uses that same var/const top-init helper, so evaluator and consteval no longer assemble named top-init functions through separate declarator-decoding logic.
- Those top-init helpers can now also preserve explicit declarator name spans when the caller knows them, which makes top-level initializer MIR closer to a backend-facing function table instead of anonymous expression wrappers.
- That explicit naming now also covers zero-initialized top vars, not just initialized top vars and top consts.
- Statement lowering: `src/mir_lower_stmt.c` now lowers a narrow simple-function subset into a one-function `SLMirProgram` for evaluator-first runtime migration.
- Program builder: `src/mir.c` now also exposes `SLMirProgramBuilder` helpers for assembling backend-facing MIR programs incrementally.
- Program validation: `src/mir.c` also exposes `SLMirValidateProgram(...)` for backend-facing sanity checks on function ranges and table references.
- IR shape: `src/mir.h` defines a compact stack-machine instruction format.
- Interpreter core: `src/mir_exec.c` executes MIR chunks.
- Function executor: `src/mir_exec.c` now also exposes `SLMirEvalFunction(...)` over `SLMirProgram`.
- `mir_exec` now also has a real `CALL_FN` execution path for same-program MIR calls, with the current local-slot and parameter-binding semantics.
- `mir_exec` now also has local-slot frame storage for MIR functions, with parameter binding into the first `paramCount` local slots and execution support for `LOCAL_LOAD` / `LOCAL_STORE`.
- `mir_exec` now also supports local address-taking and simple reference dereference through `LOCAL_ADDR`, `DEREF_LOAD`, and `DEREF_STORE`.
- `mir_exec` now also has a backend-neutral index hook in `SLMirExecEnv`, which the evaluator uses for array/slice-style indexing without baking evaluator-private storage types into MIR execution.
- `mir_exec` now also has a backend-neutral indexed-address hook in `SLMirExecEnv`, which the evaluator uses for element references (`ARRAY_ADDR`) and simple `arr[i] = rhs` lowering on the MIR path.
- `mir_exec` now also has backend-neutral iterator hooks in `SLMirExecEnv`, which the evaluator uses for MIR `for in` lowering across both synthetic sequence iteration and iterator-protocol sources.
- `mir_exec` now also has backend-neutral aggregate field hooks in `SLMirExecEnv`, which the evaluator uses for `AGG_GET` / `AGG_ADDR` without exposing evaluator-private aggregate layout to MIR execution.
- When lowered field metadata exists, `mir_exec` resolves aggregate field names through `program.fields[]` before falling back to instruction spans.
- `mir_exec` now also executes basic control-flow ops: `JUMP`, `JUMP_IF_FALSE`, and `RETURN_VOID`.
- `mir_exec` now also executes `CALL_HOST` through the explicit `SLMirExecEnv.hostCall` bridge.
- `mir_exec` now also materializes `SLMirConst_FUNCTION` and executes `CALL_INDIRECT` for same-program function references.
- The MIR runtime path now also has focused evaluator integration coverage for statement-lowered indirect function-value calls.
- `mir_exec` now also executes `LOCAL_ZERO` through an explicit `SLMirExecEnv.zeroInitLocal` hook.
- Expression MIR now also lowers plain builtin `len(x)` calls to `SLMirOp_SEQ_LEN` instead of leaving them on generic name-resolved `CALL`.
- Expression MIR now also lowers builtin `cstr(x)` and receiver sugar `x.cstr()` to `SLMirOp_STR_CSTR`, so that pure builtin no longer depends on generic evaluator call resolution.
- Lowered MIR programs now also rewrite plain builtin `copy(dst, src)` calls to `CALL_HOST`, so the current runtime byte-copy behavior can be explicit on MIR even before slice/view lowering is fully migrated.
- Lowered MIR programs now also rewrite plain builtin `print(...)` calls to `CALL_HOST` through `program.hosts[]`, instead of leaving that builtin on generic call-name resolution.
- Lowered MIR programs now also rewrite plain builtin `concat(...)` calls to `CALL_HOST`, so string concatenation crosses the MIR host boundary explicitly instead of staying on generic call-name resolution.
- Lowered MIR programs now also rewrite plain builtin `free(...)` calls to `CALL_HOST`, so allocation cleanup crosses the MIR host boundary explicitly instead of staying on generic call-name resolution.
- CTFE wrapper: `src/ctfe.c` lowers expressions through `src/mir_lower.c` and delegates execution to `src/mir_exec.c`.
- The direct-expression CTFE wrapper in `src/ctfe.c` now also accepts optional tuple/index hooks, so evaluator and typechecker can keep tuple literals and tuple argument expressions on MIR instead of forcing expression-level fallback.
- The direct-expression CTFE wrapper in `src/ctfe.c` now also accepts optional aggregate field hooks, so evaluator and typechecker can keep aggregate field reads on MIR instead of forcing expression-level fallback for `x.field` on MIR-owned values.
- The consteval path in `src/typecheck/consteval.c` now also tries MIR-first execution for simple const function bodies before falling back to `ctfe_exec`.
- The const-block path in `src/typecheck/resolve.c` now also tries MIR-first execution for simple `const { ... }` blocks before falling back to `ctfe_exec`.
- The consteval-side MIR env now also supplies typed zero-init and value-coercion hooks, so simple typed locals and scalar/optional return adaptation can stay on the MIR path.
- The consteval-side MIR env now also supplies tuple materialization and tuple indexing hooks, so simple const-call paths can keep tuple returns and grouped tuple decomposition on MIR instead of forcing immediate fallback.
- The consteval-side MIR env now also supplies aggregate field hooks, which keeps local aggregate field reads/writes and aggregate-valued helper call results on MIR for both top-level const evaluation and `const { ... }` blocks.
- Consteval identifier/field resolution now also materializes imported package aliases as conservative const package-value sentinels, so direct const expressions and `const { ... }` local aliases like `const f = pkg.Helper` can stay const-evaluable without depending on generic identifier failure for `pkg`.
- The consteval MIR function-body lowering path no longer pre-rejects imported-package field initializers in local `const` bindings, so those shapes now fall through to the normal MIR-lowering and fallback boundary instead of bailing out before lowering starts.
- Evaluator root top-init program formation now starts through `src/mir_lower_pkg.c`'s named begin-program helper instead of a private evaluator-side append wrapper, which keeps one more package-entry formation step on the shared MIR-lowering boundary.
- The consteval-side MIR env now also supplies sequence-length and iterator hooks for simple `for in`, covering both string/tuple-style sequence iteration and iterator-protocol sources resolved through the shared typechecker hook-selection logic.
- That sequence `for in` coverage now also supports `for &item in ...` over string and tuple-backed MIR sequence sources in consteval, so const functions and `const { ... }` blocks can keep simple ref iteration on MIR instead of dropping back to `ctfe_exec`.
- Consteval identifier resolution now also materializes plain function names as MIR-compatible function-reference values, so top-level consts and const-call bodies can keep local function-value aliases on the MIR path instead of dropping back to non-const identifier lookup.
- The current consteval MIR subset also covers simple non-`for in` `for` loops and successful `assert` statements with trailing message/format arguments, and both const functions and `const { ... }` blocks now have focused regression coverage for those shapes.
- That loop coverage now also pins `break` and `continue` behavior in simple consteval MIR `for` loops, for both top-level const calls and `const { ... }` blocks.
- The simple const-call path now also rewrites safe plain direct calls into same-program `CALL_FN` edges, so small const call chains can stay on MIR instead of re-entering `ctfe_exec` one function at a time.
- That direct-call rewrite now resolves conservative plain calls through the shared checked call resolver (`SLTCResolveCallByName(...)`) instead of only arity/name scanning, so supported MIR const calls can stay aligned with normal call resolution as package and overload handling expand.
- The simple const-call path now also rewrites simple ungrouped top-level const identifier loads into zero-arg `CALL_FN` edges, so those supported const dependencies no longer have to stay on generic `resolveIdent` when the surrounding const function is already on MIR.
- The simple const-call path now also rewrites direct calls through simple top-level const function-value aliases to `CALL_FN`, so those supported const calls no longer have to stay on generic `resolveCall`.
- The simple const-block MIR path now also rewrites safe plain direct helper calls into same-program `CALL_FN` edges, so `const { ... }` blocks no longer have to fall back to per-call `resolveCall` when they stay within that conservative subset.
- The const-block MIR path now also rewrites conservative imported package selector calls inside MIR-lowered helper functions through the shared typechecker package-method resolver, so supported `pkg.F(...)` helper calls in `const { ... }` no longer have to stay on generic consteval call lookup.
- The const-block MIR executor path now also drops `resolveIdent`, `resolveCall`, and `resolveCtx` entirely when a lowered MIR program has no remaining generic `LOAD_IDENT` / `CALL` instructions, so fully lowered const blocks no longer keep that dynamic-resolution callback dependency alive.
- The evaluator top-init path now also rewrites safe plain direct calls from the root initializer MIR function into same-program `CALL_FN` edges, so simple top-level initializer call chains can stay inside one MIR program.
- The evaluator top-init path now also rewrites conservative imported package selector calls in the root initializer expression to `CALL_FN`, so direct top-level initializer package calls no longer have to stay as generic evaluator-resolved `CALL`.
- The evaluator top-init path now also rewrites simple top-level identifier dependencies to zero-arg `CALL_FN` edges, so same-package top const/top var init chains can stay inside one MIR program instead of falling back to `resolveIdent`.
- The evaluator now assembles root top-init programs, top const init functions, and top var init functions through one shared append-and-rewrite helper, which reduces one-off top-init orchestration while `mir_lower_pkg` grows toward a fuller package-level lowering boundary.
- The evaluator top-init path now also rewrites simple same-file function identifier loads in the root initializer MIR function to `PUSH_CONST FUNCTION`, so top-level function values do not have to stay on generic `LOAD_IDENT` resolution.
- Those top-init identifier and top-level function-alias rewrites are now package-scoped, so duplicate names in imported packages do not leak into current-package MIR lowering.
- The evaluator top-init path now also rewrites conservative calls through top-level function-value aliases directly to `CALL_FN` when the alias target is statically known, so nonzero-arg alias calls stay on MIR too.
- The evaluator top-init path now also rewrites conservative zero-arg calls through top-level function-value aliases by inserting `PUSH_CONST FUNCTION` plus `CALL_INDIRECT`, which is the first MIR rewrite here that needs instruction insertion rather than a one-op replacement.
- Evaluator top-level const and top-level var execution now try that explicit top-init MIR program path first, instead of starting on the raw expression-MIR path and only reaching top-init MIR when referenced from some other MIR-owned function.
- The evaluator runtime MIR path now also rewrites simple same-file top-const identifier loads in lowered functions to zero-arg `CALL_FN` edges, so those dependencies no longer have to stay on generic `LOAD_IDENT` resolution when the function body is already on MIR.
- The evaluator runtime MIR path now also rewrites simple same-package top-var identifier loads in lowered functions to zero-arg `CALL_FN` edges, so supported top-level var dependencies no longer have to stay on generic `resolveIdent` when the function body is already on MIR.
- The evaluator runtime MIR path now also rewrites supported package-qualified function-value loads like `pkg.helper` into explicit MIR function refs in lowered functions and top-init code, so those paths no longer depend on package-ref field access at execution time.
- The evaluator runtime MIR path now also rewrites simple same-file function identifier loads in lowered functions to `PUSH_CONST FUNCTION`, so function values and indirect calls do not have to stay on generic `LOAD_IDENT` resolution when that function body is already on MIR.
- Those runtime identifier and top-level function-alias rewrites are now package-scoped too, so duplicate imported names do not change which current-package helper or alias MIR lowers to.
- The evaluator runtime MIR path now also rewrites conservative calls through top-level function-value aliases directly to `CALL_FN` when the alias target is statically known, so nonzero-arg alias calls no longer depend on generic evaluator call resolution.
- The evaluator runtime MIR path now also rewrites conservative zero-arg calls through top-level function-value aliases by inserting `PUSH_CONST FUNCTION` plus `CALL_INDIRECT`, so that case no longer has to rely on generic evaluator call resolution.
- MIR statement lowering now also accepts successful `assert` statements with trailing message or format args, since those extra operands do not affect the success path and no longer need to force fallback.
- Lowered function programs can now rewrite literal pushes into `SLMirConst` entries plus `SLMirOp_PUSH_CONST`, so function execution is less dependent on source-slice decoding.
- Lowered function programs can also rewrite `LOAD_IDENT` and direct `CALL` sites to `SLMirSymbolRef` table entries, so name metadata lives in the MIR program instead of only in instruction spans.
- Lowered call symbols now also preserve simple call-shape flags, such as selector-style calls where the receiver has already been lowered as argument `0`.
- Lowered `CAST` instructions now also intern their target types into `program.types[]`, so type-directed backends do not need to recover cast metadata from parser ASTs later.
- Lowered expression MIR now also interns `AGG_GET` / `AGG_ADDR` field names into `program.fields[]`, so aggregate field ops no longer need to depend only on source spans once an expression has been lowered to a MIR program.
- MIR programs now also have an explicit `program.hosts[]` table for hostcall metadata, and current lowered MIR requires `CALL_HOST` to reference that table instead of treating instruction `aux` as evaluator-private state.
- The evaluator now uses that host table for one real runtime case: plain builtin `print(...)` calls lowered through the simple MIR path are rewritten to `CALL_HOST`.
- The evaluator now also uses that host table for plain builtin `concat(...)` calls lowered through the simple MIR path.
- The evaluator now also uses that host table for plain builtin `copy(dst, src)` calls lowered through the simple MIR path.
- The evaluator-side direct-call rewrite now also covers conservative imported package selector calls, such as `math.Add(...)`, when the import alias and callee resolve unambiguously by arity.
- The evaluator-side hostcall rewrite now also covers conservative selector calls for `platform.exit(...)`, including import aliases such as `p.exit(...)`.
- Statement MIR lowering no longer rejects selector-shaped `CALL` instructions up front, so conservative imported package selector calls inside MIR-lowered function bodies can stay on the MIR path and be rewritten later to `CALL_FN` or `CALL_HOST`.
- The evaluator now also uses tiny one-function MIR programs for top-level zero-init vars, and tries direct MIR-function evaluation first for simple top-level initializer expressions before falling back to older expression paths.
- The evaluator-side MIR execution wiring now goes through one shared exec-context path for both function-body MIR and top-level init MIR, so per-frame source/file and call-stack bookkeeping no longer have to be reassembled separately for those two entrypoints.
- `mir_lower` now exposes the same instruction-materialization path to `mir_lower_stmt`, so statement-lowered runtime MIR also uses the same const/symbol/type tables instead of appending raw expression instructions.
- MIR programs now also carry explicit source entries and per-function `sourceRef` metadata, so execution and future backends do not need to assume one global source/file for the whole program.
- MIR programs now also carry explicit local metadata in `program.locals[]`, sliced per function by `localStart` / `localCount`.
- MIR programs also have `program.fields[]`, and statement-lowered aggregate field ops can now reference that table instead of depending only on source spans for field names.

MIR is not yet the full lowering IR for runtime execution, but the repo now carries the beginning of a backend-facing MIR program model in `src/mir.h`. The extra function/program/metadata structs and runtime-oriented opcodes are scaffolding for that migration. `src/mir_lower_stmt.c` is the first runtime-side lowering step and currently targets a deliberately small statement subset.

## Current role in the compiler

Today, MIR is used for const-evaluating expressions.

- Typechecker const-eval path (`src/typecheck.c`) calls `SLCTFEEvalExpr`, which first builds MIR and then interprets it.
- C backend const folding (`src/codegen_c.c`) uses `SLConstEvalSession*` APIs; those APIs route through the same const-eval path and therefore use MIR for expression evaluation.
- The CLI evaluator path (`src/evaluator.c`) also calls `SLCTFEEvalExpr` for expression evaluation inside its execution engine.

MIR is still mostly expression-first. Statement/block/control-flow const execution is handled by `src/ctfe_exec.c`, but the evaluator now attempts MIR execution first for a simple function-body subset before falling back.

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

For backend-facing `SLMirProgram` lowering, instructions also have a 32-bit `aux` operand slot. It is currently used by `SLMirOp_PUSH_CONST` to reference entries in the program constant pool.
The same `aux` slot is also used by lowered identifier/call instructions to reference `program.symbols[]`.
For lowered `CAST` instructions, `aux` references `program.types[]`.
For lowered `CALL_HOST` instructions, `aux` references `program.hosts[]`.
For lowered `AGG_GET` and `AGG_ADDR`, `aux` references `program.fields[]`, so field metadata travels with the MIR program instead of being recovered only from parser spans.
For `JUMP` and `JUMP_IF_FALSE`, `aux` is a function-local instruction index within the current `SLMirFunction`.
Each `SLMirFunction` now also points at `program.sources[function.sourceRef]`, which is how runtime execution keeps the active source/file context aligned with the current MIR frame.
Each `SLMirFunction` also points at its local metadata slice in `program.locals[]`, and each `SLMirLocal` currently carries a `typeRef` plus flags such as `PARAM`, `MUTABLE`, and `ZERO_INIT`.
For lowered `CALL_FN` and `CALL_HOST` instructions that came from selector syntax with a synthetic receiver in argument slot `0`, the high bit of `tok` is now used as `SLMirCallArgFlag_RECEIVER_ARG0`, so execution can drop that receiver before invoking the resolved target.

## Opcode set (current)

From `SLMirOp` in `src/mir.h`:

- `SLMirOp_PUSH_INT`
- `SLMirOp_PUSH_FLOAT`
- `SLMirOp_PUSH_BOOL`
- `SLMirOp_PUSH_STRING`
- `SLMirOp_PUSH_NULL`
- `SLMirOp_PUSH_CONST`
- `SLMirOp_LOAD_IDENT`
- `SLMirOp_CALL`
- `SLMirOp_CALL_FN`
- `SLMirOp_CALL_HOST`
- `SLMirOp_CALL_INDIRECT`
- `SLMirOp_LOCAL_ZERO`
- `SLMirOp_LOCAL_LOAD`
- `SLMirOp_LOCAL_STORE`
- `SLMirOp_LOCAL_ADDR`
- `SLMirOp_DROP`
- `SLMirOp_JUMP`
- `SLMirOp_JUMP_IF_FALSE`
- `SLMirOp_ASSERT`
- `SLMirOp_DEREF_LOAD`
- `SLMirOp_DEREF_STORE`
- `SLMirOp_UNARY`
- `SLMirOp_BINARY`
- `SLMirOp_INDEX` (element index; non-slice form; strings handled directly, backend-specific containers can go through the index hook)
- `SLMirOp_ITER_INIT`
- `SLMirOp_ITER_NEXT`
- `SLMirOp_RETURN`
- `SLMirOp_RETURN_VOID`

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

1. Lowers the expression through `SLMirLowerExprAsFunction(...)`.
2. If unsupported, returns `0` with `*outIsConst = 0`.
3. Otherwise delegates to `SLMirEvalFunction(...)` in `src/mir_exec.c`.

Interpreter details:

- Raw expression chunks still decode literal values from source slices (`start`/`end`) at execution time.
- Lowered function programs may instead materialize those literals in `program.consts[]` and execute them via `SLMirOp_PUSH_CONST`.
- `SLCTFEEvalExpr` now adapts its CTFE callbacks into `SLMirExecEnv`.
- `SLMirEvalFunction(...)` validates the MIR program shape before execution.
- `SLMirProgramNeedsDynamicResolution(...)` reports whether a MIR program still contains generic `LOAD_IDENT` / `CALL` instructions.
- `LOAD_IDENT` is resolved by `SLMirResolveIdentFn`.
- `CALL` is resolved by `SLMirResolveCallFn` with popped arguments in source order.
- `INDEX` handles strings directly in `mir_exec` and can delegate other container indexing to `SLMirExecEnv.indexValue(...)`.
- `ITER_INIT` and `ITER_NEXT` execute through backend-neutral iterator hooks in `SLMirExecEnv`.
- `CALL_FN` now executes same-program MIR functions through the function executor. Today that path is intentionally narrow and only supports the current arg-less/simple case until local slots and parameter binding move into MIR.
- `CALL_FN` now supports passing arguments into same-program MIR functions, with those arguments bound to the first `paramCount` local slots.
- When `SLMirCallArgFlag_RECEIVER_ARG0` is set on lowered `CALL_FN` or `CALL_HOST`, execution drops argument `0` before invoking the resolved target.
- `CALL_HOST` invokes `SLMirExecEnv.hostCall(hostCtx, hostId, args, argCount, ...)`, where `hostId` comes from `program.hosts[aux].target`.
- `SLMirConst_FUNCTION` currently materializes as a same-program function reference value.
- `CALL_INDIRECT` currently expects the callee value to have been pushed before its arguments, then invokes the referenced same-program MIR function.
- Statement MIR rewrite now also turns local-identifier calls like `f(...)` into `CALL_INDIRECT` when `f` resolves to a local slot, so local function values do not need to fall back to evaluator name resolution.
- `TUPLE_MAKE` delegates tuple materialization through `SLMirExecEnv.makeTuple(...)`, so tuple storage stays owned by the embedding runtime instead of `mir_exec`.
- `LOCAL_LOAD` and `LOCAL_STORE` execute against per-frame local storage.
- `LOCAL_ADDR` currently materializes a reference to a MIR local slot.
- `DEREF_LOAD` and `DEREF_STORE` currently operate on those reference values.
- `LOCAL_ZERO` now resolves the target local's `typeRef` through `program.locals[]` and delegates zero-value materialization through `SLMirExecEnv.zeroInitLocal(...)`.
- `DROP` pops and discards one stack value.
- `JUMP` and `JUMP_IF_FALSE` execute using function-local instruction indices stored in `aux`.
- `JUMP_IF_FALSE` currently coerces its popped condition through the existing MIR boolean-cast rules.
- `SLMirExecEnv.backwardJumpLimit` can cap taken backward jumps per MIR frame. The typechecker now uses that to keep MIR consteval from spinning forever on non-progressing loops, then falls back to the older CTFE path for the final user-facing diagnostic.
- When lowered symbol metadata exists, `mir_exec` resolves identifier/call names through `program.symbols[]` before falling back to instruction spans.
- The evaluator and consteval now also drop `resolveIdent` / `resolveCall` entirely when a MIR program has no remaining generic `LOAD_IDENT` / `CALL` instructions, so fully lowered MIR paths do not silently keep that callback dependency alive.
- The expression-level CTFE wrapper in `src/ctfe.c` now does the same callback drop for fully lowered MIR expressions, so direct expression evaluation no longer keeps generic identifier/call callbacks alive when the MIR program does not need them.
- For direct call symbols, `program.symbols[]` now also carries lightweight call-shape flags that future backends can use when deciding between plain calls, method-style lowering, or host shims.
- `CAST` retains both its current scalar cast opcode token and an explicit type-table reference for backend consumers.
- `mir_exec` now also switches function source context per MIR frame through `function.sourceRef`, and can notify embedders through `SLMirExecEnv.enterFunction` / `leaveFunction`.
- The evaluator now uses those frame hooks to keep MIR-owned calls visible in its normal call stack, so recursion/depth-sensitive fallback paths continue to observe the right dynamic call context while runtime execution migrates onto MIR.
- `RETURN` expects exactly one stack value; then sets `*outIsConst = 1`.
- `RETURN_VOID` completes a MIR function without requiring a stack value.

Return behavior:

- `0` + `*outIsConst = 1`: const value produced.
- `0` + `*outIsConst = 0`: not const-evaluable in current context/subset.
- `-1`: hard failure (for example OOM or decoding diagnostic errors).

## Interaction with consteval and `ctfe_exec`

Const-eval in the typechecker is layered:

- `SLTCEvalConstExprNode` handles special expression forms directly (`sizeof`, `cast`).
- For the remaining expression subset, it calls `SLCTFEEvalExpr` (MIR path).
- For const function bodies/statements, `SLTCResolveConstCall` now tries MIR first and then falls back to `SLCTFEExecEvalBlock` (`src/ctfe_exec.c`).
- For `const { ... }` blocks, `src/typecheck/resolve.c` now also tries MIR first and then falls back to `SLCTFEExecEvalBlock`.
- `ctfe_exec` delegates expression evaluation back through callbacks to `SLTCEvalConstExprNode`, which usually routes to MIR.

So today:

- MIR is the expression evaluator backend.
- `ctfe_exec` is still the statement/control-flow evaluator backend.
- `mir_exec` is now the dedicated MIR execution module that future runtime MIR work should extend instead of growing `ctfe.c` or `evaluator.c`.
- `mir_lower` is now the dedicated MIR lowering boundary for expression-to-program lowering, and should grow into checked-program/function lowering instead of adding more MIR assembly logic to `ctfe.c`.
- `mir_lower_pkg` is the first explicit package/top-init lowering boundary on the runtime side, so evaluator top-level initialization no longer needs to treat zero-init and initializer lowering as an implicit extension of expression lowering.
- `mir_lower_stmt` is the first function-body lowering step on the runtime side. Today it only handles a narrow simple subset:
  - parameters mapped to typed local slots
  - single-name `var`/`const` declarations with initializer expressions
  - typed single-name declarations without initializer, lowered as `LOCAL_ZERO`
  - conservative grouped local declarations with typed zero-init or one initializer per declared name
  - local `&name`, `*name`, and `*name = value` forms where `name` is a MIR local
  - conservative `*expr`, `*expr = rhs`, and `*expr op= rhs` forms where `expr` is a replayable pointer/ref expression such as a field or element access
  - conservative `&arr[i]` and plain `arr[i] = rhs` forms lowered through `ARRAY_ADDR`
  - conservative `arr[i] op= rhs` forms lowered by replaying side-effect-free lvalues through `INDEX` plus `ARRAY_ADDR`
  - conservative `x.field`, `&x.field`, and plain `x.field = rhs` forms lowered through `AGG_GET` / `AGG_ADDR`
  - conservative `x.field op= rhs` forms lowered by replaying side-effect-free lvalues through `AGG_GET` plus `AGG_ADDR`
  - plain builtin `print(...)` rewritten to `CALL_HOST`
  - plain builtin `copy(dst, src)` rewritten to `CALL_HOST`
  - plain builtin `concat(...)` rewritten to `CALL_HOST`
  - builtin `cstr(x)` and `x.cstr()` lowered to `STR_CSTR`
  - conservative imported package calls like `pkg.F(...)`, lowered to `CALL_FN` when the target is unambiguous and non-variadic
  - conservative `platform.exit(...)` selector calls rewritten to `CALL_HOST`
  - simple local assignment and compound assignment
  - equal-count multi-assign lowered through temp locals so RHS evaluation stays separate from LHS stores
  - single-RHS tuple/multi-return decomposition in grouped declarations and multi-assign, lowered through temp locals plus `INDEX`
  - tuple expressions and tuple-valued `return a, b, ...`, lowered through `TUPLE_MAKE`
  - expression statements
  - `if` / `else`
  - successful `assert` statements
  - conservative block-scoped `defer`, including reverse-order execution and replay on `return`, `break`, and `continue` when unwinding out of active nested scopes
  - simple non-`for in` `for` loops, plus `break` and `continue`
  - conservative `for in` lowering through MIR iterator hooks, covering both sequence sources and iterator-protocol sources, with key/value or value-only identifier bindings and discard
  - conservative `switch` lowering for plain subject/condition switches with case labels, `default`, switch-local `break`, and subject-pattern alias bindings
  - `return`
  - nested blocks
- The evaluator now tries that MIR function-body path before falling back to `ctfe_exec`, which keeps runtime behavior stable while the MIR subset grows.
- That MIR lowering path now keeps “already has a MIR function index” separate from “still lowering”, so direct recursive calls can lower to `CALL_FN` instead of forcing the whole function back to the AST runtime path.
- That evaluator-side MIR path now also lowers unambiguous plain direct calls into same-program `CALL_FN` edges. The remaining fallback cases are still variadic, ambiguous, and true receiver-style calls. Supported builtin/package calls that already have explicit MIR rewrites include `print(...)`, `copy(dst, src)`, `concat(...)`, `free(...)`, and `platform.exit(...)`.
- The new per-function source identity is what makes that broader direct-call lowering possible without depending on one evaluator-global `currentFile`/source view for the whole MIR program.
- The `SLMirExecEnv` boundary is intentionally MIR-native so future backends, including a Wasm backend, can reuse MIR lowering/execution contracts without depending on CTFE-specific callback names.
- The new function-level executor boundary means future backends can target `SLMirProgram` functions directly instead of raw expression chunks.
- The initial `CALL_FN` support means that boundary is no longer just an entry point; MIR function-to-function execution has started to exist, even though full frame/locals/param semantics are still ahead.
- Local-slot execution is the next step in that direction: MIR functions can now carry state in frame slots, typed zero-init now uses MIR local metadata, and local address-taking works, but richer lvalue semantics beyond plain local refs are still ahead.
- Basic control flow is now part of the MIR executor contract too, which is necessary before checked function bodies can migrate off the AST/`ctfe_exec` path.
- `mir_lower_stmt` now also tracks breakable vs continuable control scopes separately, so MIR-lowered `break` can target the innermost loop or switch while `continue` still finds the nearest loop.
- The explicit hostcall path is another backend-facing step: non-pure operations no longer have to masquerade as generic name-resolved calls once MIR lowering starts using `CALL_HOST`.
- The indirect-call path is the same kind of step for function values: MIR now has a backend-visible function-reference form instead of leaving all such execution to evaluator-specific machinery.
- The constant-pool rewrite is the first step away from MIR depending on parser source text at execution time, which is important for a future Wasm backend or any serialized MIR consumer.
- The symbol-table rewrite does the same for simple name resolution metadata: a backend can inspect imports/calls/idents from MIR program tables instead of reverse-engineering them from parser offsets.
- That symbol metadata now also preserves one small but useful execution detail: whether a lowered direct call came from selector syntax and already includes the receiver as argument `0`.
- The type-table rewrite does the same for cast targets: a backend can inspect cast target metadata from MIR tables instead of reconstructing it from AST shape at codegen time.
- The host-table rewrite is the same kind of cleanup for host-backed operations: future backends can inspect hostcall metadata from MIR tables instead of treating `CALL_HOST` operands as evaluator-private numbers.
- `SLMirValidateProgram(...)` makes those table contracts explicit, which is useful before adding more backends that will consume MIR directly instead of relying on evaluator fallbacks.

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
3. Update lowering behavior in `src/mir_lower.c` if the program/function boundary changes.
4. Update interpreter behavior in `src/mir_exec.c` and integration in `src/ctfe.c`.
5. Update const-eval integration paths in `src/typecheck.c` and any caller-specific resolvers.
6. Add tests that cover both:
   - supported paths (`outSupported = 1`, `outIsConst = 1`)
   - fallback paths (`outSupported = 0` or `outIsConst = 0`) without hard errors.
