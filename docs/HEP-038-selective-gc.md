# HEP-38 selective precise GC

Status: Draft

## Summary

HEP-38 changes heap allocation semantics to support selective GC without introducing a new pointer
type or alloc allocation syntax.

The rule is:

- plain `alloc` allocates in the GC heap
- explicit `alloc ... in allocator` allocates manual allocator-managed memory

Examples:

```hop
var a *int  = alloc int
var b *int  = alloc int in context.allocator
var c *Vec2 = alloc Vec2{ x: 1, y: 2 }
var d *Vec2 = alloc Vec2{ x: 1, y: 2 } in context.allocator

dealloc b
dealloc d
```

GC-managed values and manual heap values both use ordinary `*T`.
The distinction is allocation provenance, not a separate source-level pointer type.

The initial implementation target is:

- precise GC in the evaluator
- precise GC in the Wasm backend/runtime
- no GC support in the C backend yet

## Motivation

HopHop needs to keep explicit memory management where it matters:

- performance-critical code
- allocator-specific layouts
- arenas and region allocation
- low-level runtime/platform code

It also benefits from automatic reclamation for graph-like or irregular heap structures where
manual lifetime management is error-prone and noisy.

Using plain `alloc` for GC and reserving `alloc ... in allocator` for manual memory is cleaner than
adding a second allocation sigil:

- the grammar stays almost unchanged
- manual memory stays explicit at the allocation site
- GC becomes the ergonomic default where no allocator is named
- allocator-backed code remains available where control matters

## Goals

- Add selective GC without introducing a second pointer type.
- Make plain `alloc` use GC-managed allocation.
- Keep explicit manual allocation available through `alloc ... in allocator`.
- Keep `dealloc` for manual memory.
- Catch common `dealloc` mistakes in debug builds, including:
  - calling `dealloc` on a GC allocation
  - calling `dealloc` with a different allocator than the allocation originated from
- Start with a precise collector for evaluator and Wasm.
- Keep unsupported paths explicit: C backend flows must reject GC-using programs directly.

## Non-goals

HEP-38 does not add:

- GC support in the C backend
- moving GC in the initial implementation
- incremental or concurrent collection in the initial implementation
- finalizers or destructor hooks in the initial implementation
- a static type distinction between GC and manual `*T`
- automatic management of non-memory resources such as files, sockets, handles, or locks

`defer` and explicit cleanup remain the mechanism for non-memory resources.

## Surface design

### 1. Syntax

No additional allocation syntax is required.

Existing `alloc` forms remain:

```ebnf
AllocExpr = "alloc" ( "[" Type Expr "]" | Type [ "{" [ FieldInitList ] "}" ] ) [ "in" Expr ] .
```

The change is semantic:

- `alloc T` is GC allocation
- `alloc T{...}` is GC allocation
- `alloc [T n]` is GC allocation
- `alloc ... in allocExpr` is manual allocation through `allocExpr`

`dealloc` syntax is unchanged:

- `dealloc value`
- `dealloc value in allocExpr`

### 2. Allocation classes

Every heap allocation belongs to exactly one provenance class:

- GC-managed allocation
- manual allocator-managed allocation

Creation rules:

- plain `alloc` creates GC-managed allocations
- `alloc ... in allocExpr` creates manual allocations

### 3. Result type

Both forms return ordinary `*T`.

This is intentional.
The type system does not distinguish:

- GC `*T`
- manual `*T`

The runtime distinguishes them by allocation provenance.

## Semantics

### 1. Plain `alloc`

Plain `alloc` no longer depends on `context.allocator`.

Instead:

- `alloc T`
- `alloc T{...}`
- `alloc [T n]`

allocate in the GC heap and return `*T`.

This means plain `alloc` becomes available without requiring an ambient `Allocator`.

### 2. Explicit allocator form

`alloc ... in allocExpr` remains the explicit manual allocation form.

Examples:

```hop
var p  *Pair    = alloc Pair in context.allocator
var xs *[i32 4] = alloc [i32 4] in context.allocator
```

These allocations are not GC-managed.
They are reclaimed explicitly through `dealloc`.

### 3. `dealloc`

`dealloc` remains the deallocation operation for manual memory only.

Using `dealloc` on a GC allocation is invalid.
Using `dealloc` with the wrong allocator is also invalid.

Because GC and manual allocations intentionally share the same static type `*T`, these errors are
not rejected by the type system.

Instead, the runtime contract is:

- debug builds must validate allocation provenance in `dealloc`
- a mismatch must trap or report a direct runtime error
- optimized builds may omit this check

This catches both of the following mistakes:

```hop
var p *Vec2 = alloc Vec2
dealloc p // invalid: GC allocation
```

```hop
var a1 = context.allocator
var a2 = some_other_allocator()
var p *Node = alloc Node in a1
dealloc p in a2 // invalid: allocator mismatch
```

### 4. Debug allocator-origin checks

In debug builds, each heap allocation must carry enough origin metadata for `dealloc` to validate:

- allocation kind: GC or manual
- for manual allocations, allocator-origin identity

The exact metadata layout is implementation detail, but the check must be strong enough to detect:

- `dealloc` on GC-managed memory
- `dealloc ... in allocExpr` where `allocExpr` is not equivalent to the originating allocator
- plain `dealloc value` using `context.allocator` when the allocation came from a different allocator

This HEP does not require a particular notion of allocator identity beyond the guarantee that the
debug check correctly detects mismatched origin under the runtime's allocator model.

## Collector model

### 1. Initial collector shape

The initial collector should be:

- precise
- non-moving
- stop-the-world
- tracing

A non-moving collector keeps the first implementation compatible with:

- address identity
- pointer comparisons
- `rawptr` escape hatches
- backend/runtime simplicity

Generational, incremental, or concurrent collection can be evaluated later.

### 2. Root discovery

The collector must use precise root information supplied by the compiler/runtime.

The exact representation is backend-specific, but roots must include at least:

- live local variables and parameters whose type can hold `*T`
- live temporaries that can hold `*T`
- global/top-level storage that can hold `*T`
- fields of reachable GC objects whose static type can hold `*T`

Because GC and manual pointers share the same `*T` type, tracing must check whether a candidate
pointer value points into the GC heap before following it.

This permits:

- a `*T` local to hold either manual or GC memory
- a GC object field to contain either manual or GC pointers

Only pointers into the GC heap are traced.

### 3. Unscanned manual memory

Manual allocations are not scanned by the collector in v1.

That means storing a GC pointer inside manual or otherwise unscanned memory is invalid in v1,
including storage hidden in:

- memory obtained from `alloc ... in allocator`
- arena allocations
- foreign memory
- `rawptr` payloads
- backend/runtime buffers not registered with the collector

Reason:

- the collector cannot keep such GC objects alive if the only remaining references are stored in
  unscanned memory

This is the main tradeoff of provenance-based selective GC without a separate GC pointer type.

## Backend and runtime support

### 1. Evaluator

The evaluator is an intended first implementation target.

It should:

- implement the precise GC heap
- maintain root visibility for interpreter frames and temporaries
- lower plain `alloc` to GC allocation
- lower `alloc ... in allocExpr` to manual allocation
- implement debug `dealloc` provenance checks

### 2. Wasm

Wasm is the other intended first implementation target.

The Wasm path should:

- provide a GC heap in linear memory
- emit or derive precise root information for live pointer-carrying locals and globals
- preserve current pointer/address semantics by keeping the initial collector non-moving
- lower plain `alloc` to GC allocation
- lower explicit allocator `alloc ... in allocExpr` to manual allocation
- implement the same debug `dealloc` provenance checks when debug runtime support is enabled

### 3. C backend

The C backend does not support GC in v1.

That means any use of plain `alloc` must be rejected by:

- `hop build --output-format c`
- `hop build`
- other C-backend flows

Recommended diagnostic shape:

```text
plain alloc requires GC support, which is not available in the C backend; use `alloc ... in allocator` for manual allocation
```

Programs that use only explicit allocator forms remain valid for the C backend.

## Interaction with existing features

### 1. `alloc`

This HEP is a compatibility break.

Before HEP-38:

- plain `alloc` used the ambient or explicit allocator model

After HEP-38:

- plain `alloc` is GC allocation
- only `alloc ... in allocExpr` is manual allocation

For existing manual-memory code, the migration is mechanical:

- `alloc T` -> `alloc T in context.allocator`
- `alloc T{...}` -> `alloc T{...} in context.allocator`
- `alloc [T n]` -> `alloc [T n] in context.allocator`

or use another explicit allocator where appropriate.

### 2. `dealloc`

`dealloc` remains valid and necessary for manual memory.
It is not a valid operation on plain-`alloc` GC allocations.

### 3. `defer`

`defer` remains important.

GC only manages memory.
It does not replace explicit cleanup for:

- file descriptors
- handles
- locks
- temporary platform state
- allocator-owned substructures that are not GC-managed

### 4. `rawptr`

`rawptr` remains an escape hatch.

Using `rawptr` to hide the only remaining reference to a GC object is invalid unless the runtime
explicitly registers that storage as a root. The v1 design assumes ordinary `rawptr` storage is
unscanned.

### 5. Cycles

Unlike ARC, the collector reclaims unreachable cycles naturally.

This is one of the main reasons for preferring selective GC over ARC here.

## Diagnostics

Recommended diagnostics and runtime errors:

1. Backend error:
   - plain `alloc` used in C backend flow
2. Debug runtime error:
   - `dealloc` used on a GC allocation
3. Debug runtime error:
   - `dealloc` used with wrong allocator

## Examples

### 1. GC-managed linked structure

```hop
struct Node {
    next  ?*Node
    value i32
}

fn make_list() *Node {
    var a = alloc Node{ value: 1 }
    var b = alloc Node{ value: 2 }
    a.next = b
    return a
}
```

### 2. Explicit manual allocation

```hop
fn main() {
    var ma     = context.allocator
    var a *i32 = alloc i32 in ma
    var b *i32 = alloc i32 in ma
    *a = 10
    *b = 32
    assert *a + *b == 42
    dealloc a, b in ma
}
```

### 3. Mixed manual and GC memory

```hop
struct Buf {
    next *Node
}

fn main() {
    var buf  *Buf  = alloc Buf in context.allocator
    var node *Node = alloc Node

    // `buf` is manual and unscanned.
    // `node` is GC-managed.
    // Storing `node` in `buf.next` is invalid in v1 if that becomes the only remaining reference.
}
```

## Implementation notes

The core language and runtime work likely falls into these pieces:

1. Typechecker:
   - split `alloc` semantics by presence or absence of `in allocExpr`
   - keep result typing the same as today
2. MIR / evaluator / Wasm lowering:
   - add GC allocation lowering for plain `alloc`
   - preserve manual allocation lowering for `alloc ... in allocExpr`
3. Runtime:
   - add precise non-moving tracing collector
   - add debug provenance metadata and `dealloc` validation
4. C backend:
   - reject plain-`alloc` GC programs explicitly

## Open questions

- Whether collection should run only on allocation pressure in v1 or also at selected safe points.
- How top-level GC roots are represented in Wasm startup and shutdown paths.
- Whether debug provenance checks should also be available in some non-debug testing modes.
- Whether later revisions should add an explicit API for registering external root storage.
