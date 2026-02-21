# SLP-19 pointer and slice unification (completed)

## Status

Implemented in trunk on February 21, 2026.

Rollout mode used: hard switch (legacy `mut&`/`mut[...]` forms removed).

## Summary

SLP-19 simplifies reference-like types into two mutability forms:

- `*X` means writable access to `X`.
- `&X` means read-only access to `X`.

It also introduces `[T]` as an unsized slice type that only exists behind `*`/`&`.

```sl
// Value types
T
[T N]

// Unsized type (no by-value values)
[T]

// Reference-like types
*T
&T
*[T]
&[T]
*[T N]
&[T N]
```

This proposal supersedes the SLP-2 split between `mut&T`/`mut&[T N]` and `mut[T]`.

`sizeof(expr)` remains supported and is generalized to return actual runtime byte size for dynamic
cases (slice pointers and variable-size aggregates).

---

## Motivation

Current forms (`&`, `mut&`, `[T]`, `mut[T]`, `*[T]`) encode mutability and "view-ness" across
multiple orthogonal syntaxes. This makes code harder to read and teach.

SLP-19 keeps one mutability axis:

- writable: `*...`
- read-only: `&...`

and one sequence axis:

- fixed-size sequence by value: `[T N]`
- runtime-size sequence behind pointer/ref: `*[T]`, `&[T]`

This keeps syntax regular for both humans and tooling.

---

## Non-goals

- Rust-style borrow/lifetime checking.
- Ownership or alias exclusivity proofs.
- Changing nullability defaults in this SLP.
- Introducing a `cap` field for slices.

---

## Type system changes

### 1. Unsized slice type

`[T]` is an unsized type. It has no by-value representation.

Valid uses:

- pointee of `*` / `&` (`*[T]`, `&[T]`)
- other places explicitly marked unsized-capable by future SLPs

Invalid uses:

```sl
var x [i32]          // error
fn f(x [i32]) void   // error
fn g() [i32]         // error
```

### 2. Reference-like representations

- `*T`, `&T`, `*[T N]`, `&[T N]` are thin pointers.
- `*[T]`, `&[T]` are fat pointers with `(ptr,len)` layout.

`len` is in the fat pointer, not in pointee storage.

### 3. Mutability model

- `*X` allows writes through that view.
- `&X` disallows writes through that view.
- Read-only does not freeze underlying storage globally.

---

## Coercions and conversions

### Implicit coercions

- `[T N] -> &[T]`
- `[T N] -> *[T]` when source is mutable lvalue
- `&[T N] -> &[T]`
- `*[T N] -> *[T]`
- `*[T] -> &[T]`
- `*T -> &T`

### No implicit coercion

- `*T -> *[T]` (rejected; avoids accidental "len=1" semantics)
- `&T -> &[T]`

Explicit helper APIs can be added later for singleton-as-slice conversion.

---

## `len` semantics (normative)

`len(x)` accepts:

- `str`
- `[T N]`
- `*[T]`, `&[T]`
- `*[T N]`, `&[T N]`

Result type is `u32`.

Behavior:

- `[T N]`, `*[T N]`, `&[T N]`: compile-time constant `N`.
- `*[T]`, `&[T]`: runtime `len` field.
- `str`: existing behavior unchanged.
- `*T`, `&T`: compile error.

---

## `sizeof` semantics (normative)

SL keeps both forms:

- `sizeof(type T)`
- `sizeof(expr E)`

Result type is `uint`.

### `sizeof(type T)`

- compile-time constant
- rejected for unsized types (`[T]`)
- rejected for variable-size aggregate types (from SLP-1)

### `sizeof(expr E)`

`sizeof(expr)` is defined for all sized expression categories, with runtime lowering where needed:

- If `E` has fixed-size value type (including `[T N]`): compile-time constant.
- If `E` has type `*T` or `&T`:
  - if `T` fixed-size: compile-time constant `sizeof(type T)`
  - if `T` variable-size aggregate: runtime helper on referenced value
- If `E` has type `*[T]` or `&[T]`: runtime `len(E) * sizeof(type T)`.
- If `E` has type `*[T N]` or `&[T N]`: compile-time constant `N * sizeof(type T)`.

This chooses "support `sizeof(expr)` and return actual byte size" over rejecting non-VSS
expressions.

### Null behavior

- For `*[T]`/`&[T]`, `sizeof(expr)` depends on `len`; a well-formed null slice has `len=0`.
- For `*V`/`&V` where `V` is variable-size aggregate, runtime size requires a valid referenced
  header. In safe mode, null traps; in unsafe mode, behavior is undefined.

---

## Interaction with SLP-1 variable-size structs

SLP-1 dependent trailing fields stay unchanged:

```sl
struct Packet {
    count u32
    data  [.count]u8
}
```

Field access type changes:

- writable base (`*Packet`) gives `p.data : *[u8]`
- read-only base (`&Packet`) gives `p.data : &[u8]`

`sizeof(p)` where `p : *Packet` or `p : &Packet` remains runtime.

---

## Migration from SLP-2 forms

- `mut&T` -> `*T`
- `mut&[T N]` -> `*[T N]`
- `mut[T]` -> `*[T]`
- `[T]` (old read-only slice value) -> `&[T]`
- `&[T N]`, `*[T]`, `*[T N]`, `&T`, `*T` remain, but with unified mutability meaning

Source migration can be mostly mechanical.

---

## Examples

```sl
fn sum(v &[i32]) i32 {
    var acc i32
    for var i = 0; i < len(v); i++ {
        acc += v[i]
    }
    return acc
}

fn scale_in_place(v *[i32], k i32) void {
    for var i = 0; i < len(v); i++ {
        v[i] *= k
    }
}

fn first(x *i32) i32 {
    return *x
}

fn main() {
    var a [i32 3] = {1, 2, 3}
    var ro &[i32] = a
    var rw *[i32] = a
    assert len(ro) == 3
    assert sizeof(ro) == 3 * sizeof(i32)
    scale_in_place(rw, 2)
    assert sum(ro) == 12
}
```

---

## Implementation result

1. Parser/type syntax:
   - `[T N]` kept as fixed-size array value type.
   - `[T]` is parsed as unsized and rejected by-value in semantic checks.
   - `*[T]`/`&[T]` are supported directly.
   - legacy `mut&` and `mut[...]` forms are rejected.
2. Typechecker:
   - by-value unsized declarations/params/returns are rejected.
   - coercions listed in this SLP are implemented.
3. Codegen/runtime:
   - `*[T]`/`&[T]` lower to `(ptr,len)` runtime representation.
   - mutable slice runtime representation no longer carries `cap`.
   - `sizeof(expr)` lowering follows the dynamic branches defined above.
4. Validation:
   - full project test suite passes with SLP-19 behavior.
5. Platform context alignment:
   - built-in `platform.Context.mem` uses `*__sl_MemAllocator`.
   - implicit `main` context fallback for `mem` is pointer-typed.
   - this removes legacy `mut&` assumptions during context compatibility checks.

Detailed task tracking: `docs/SLP-19.1-implementation-checklist.md` (completed).

## Resolved decisions

1. Nullability policy for pointers and refs:
   - unchanged in SLP-19; non-null-by-default is deferred to a dedicated future SLP.
2. `sizeof(expr)` on null pointer to variable-size aggregate:
   - safe mode traps/panics.
   - unsafe mode is undefined behavior.
