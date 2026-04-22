# HEP-2 types, pointers, references and slices (completed)

## Summary

HEP-2 introduces:

- updated array syntax: `[T N]` and `[T .len]`
- pointer, reference, and slice type forms
- mutability via `mut` (read-only by default for refs/slices)
- slice expressions with compile-time/runtime bounds checks
- `new` allocation forms with explicit allocator argument

For now, this is intentionally C-like:

- `*` and `&` may both be null
- no Rust-style lifetime/escape enforcement
- ownership meaning is semantic/conventional (`*` = owned, `&` = borrowed)

---

## Non-goals

HEP-2 explicitly does **not** attempt the following:

- Full alias analysis / alias prevention.
- Rust-style borrow checking.
- Escape/lifetime enforcement for references/slices.
- Proof of unique mutable access.

Rationale:

- avoiding harmful aliasing globally is a major, separate effort
- HEP-2 focuses on type forms, mutability model, slicing semantics, and ABI mapping
- safety remains C-like for now, with explicit programmer responsibility

Read-only view types constrain writes through that specific view only; they do not freeze underlying
storage globally.

---

## Type forms

```hop
// Value types
T         // one value
[T N]     // array of N values
[T .len]  // dependent-length array type form (where applicable)

// Pointer types (owned)
*T        // pointer to one value
*[T]      // pointer to runtime-length array (ptr + len)
*[T N]    // pointer to fixed-length array

// Reference types (borrowed)
&T         // read-only reference to one value
mut&T      // mutable reference to one value
&[T N]     // read-only reference to fixed-length array
mut&[T N]  // mutable reference to fixed-length array

// Slice/view types
[T]        // read-only slice (ptr + len)
mut[T]     // mutable slice (ptr + len + cap)
```

### Nullability (current vs future)

- Current: `*` and `&` values may be null.
- Future HEP: add `?` optional-value syntax:
  - `?*T` may be null
  - `*T` non-null

---

## Semantics of `*` vs `&`

`*` and `&` are concretely the same low-level concept (addressable memory reference), but with
different semantic meaning:

- `*` means memory managed by me (owned)
- `&` means memory managed elsewhere (borrowed)

The compiler does not attempt Rust-like lifetime/escape checks in HEP-2.

---

## Address-of and addressability

`&expr` is valid for expressions that:

1. already reside in memory, or
2. can be trivially moved to memory (e.g. literal values).

This includes function arguments:

```hop
fn f(x &i32) void
f(&3) // materialize temporary storage for 3 and pass its address
```

This follows C-style responsibility and footgun profile.

---

## Assignment and dereference rules

Reference/slice variables do not auto-dereference on assignment.

Given:

```hop
var b i32
var c &i32
var r &i32 = &b
```

Then:

- `r = c` is valid (same type assignment)
- `r = &b` is valid
- `r = 1` is invalid (type mismatch)

To write through a one-value reference, use explicit dereference:

```hop
var mr mut&i32 = &b
*mr = 2 // write through reference
```

For slices/arrays, writes happen through indexing and require mutable view type:

```hop
var xs [i32] = ...
xs[0] = 1 // error (read-only)

var ys mut[i32] = ...
ys[0] = 1 // ok
```

---

## Slice syntax and semantics

### Syntax

Slice expression:

```ebnf
SliceExpr = PostfixExpr "[" Start? ":" End? "]" ;
Start     = Expr ; // positive-value integer expression
End       = Expr ; // positive-value integer expression
```

### Semantics

- `start` is inclusive
- `end` is exclusive
- omitted indices:
  - `a[:]`  = `a[0:len(a)]`
  - `a[n:]` = `a[n:len(a)]` (result length is `len(a) - n`)
  - `a[:m]` = `a[0:m]`

### Bounds checks

- If `start` and `end` are compile-time constants: check at compile time.
- Otherwise: emit runtime checks.
- Out-of-bounds access panics.

### Safe mode

- `hop` has a safe mode, on by default:
  - inserts bounds checks for memory accesses that may go out of bounds
  - compiles generated C with UBSan when available (overflow/alignment traps)
- `hop --unsafe` disables safe-mode inserts.

---

## Mutability model

- References and slices are read-only by default.
- `mut` is required for writable view/reference types.
- Read-only view types (`&T`, `&[T N]`, `[T]`) constrain writes through that specific view.
- They do not globally freeze underlying storage; another mutable alias may still write.

Valid mutability forms:

- `mut&T`
- `mut&[T N]`
- `mut[T]`

Invalid forms in HEP-2:

- `mut*T`
- `mut T`

`const` is not a type qualifier in HEP-2.

---

## Equality and ordering

No built-in comparison support is added in HEP-2.

Ordering/comparison for arrays/slices/pointers is deferred to a separate HEP.

---

## Default / zero value behavior

- Pointer/reference zero value is null.
- `len` on null pointer/reference/slice-like values returns 0.

Future with `?` nullability:

- non-null reference fields in by-value aggregates must be initialized before use
- pointer-allocated values (`new`) may start with null fields and require programmer initialization

Static analysis for definite initialization can be added later.

---

## Allocation API (`new`)

Logical signatures (current):

```hop
fn new(ma mut&MemAllocator, type T) *T
fn new(ma mut&MemAllocator, type T, N uint) *[T N] // logical dependent return shape
```

Logical signatures (future with `?`):

```hop
fn new(ma mut&MemAllocator, type T) ?*T
fn new(ma mut&MemAllocator, type T, N uint) ?*[T N]

// non-null forms that panic on allocation failure
fn new(ma mut&MemAllocator, type T) *T
fn new(ma mut&MemAllocator, type T, N uint) *[T N]
```

Ownership:

- caller owns result and must free when done.

---

## `str` in HEP-2

Type aliases are a separate HEP and out of scope here.

For HEP-2, treat `str` as:

- `str`  = `[u8]`
- `*str` = `*[u8]`
- `mut str` = `mut[u8]` (if/when alias qualifiers are defined)

In practice, most APIs should prefer read-only views for input text.

---

## Canonical type grammar and precedence

### EBNF

```ebnf
Type            = MutRefType
                | RefType
                | MutSliceType
                | SliceType
                | PtrType
                | ArrayType
                | DepArrayType
                | TypeName ;

MutRefType      = "mut" "&" Type ;
RefType         = "&" Type ;

MutSliceType    = "mut" "[" Type "]" ;
SliceType       = "[" Type "]" ;

PtrType         = "*" Type ;

ArrayType       = "[" Type ConstExpr "]" ;
DepArrayType    = "[" Type "." Identifier "]" ;

TypeName        = Identifier { "." Identifier } ;
```

### Precedence/association

- Type constructors are right-associative:
  - `*&T` parses as `*( &T )`
  - `&*T` parses as `&( *T )`
- `mut` binds to the immediately following type constructor.

---

## Representation and ABI mapping (current C backend)

C backend target representation:

```c
typedef struct { const void* ptr; __hop_uint len; } hop_slice_ro;      // [T]
typedef struct { void* ptr; __hop_uint len; __hop_uint cap; } hop_slice_mut; // mut[T]
```

Fixed-array refs and pointers:

- `&[T N]`, `mut&[T N]`, `*[T N]` lower to `T*` at C ABI level.
- Length `N` remains known in HopHop typing/codegen (not an extra runtime field).

Pointer-to-slice forms:

- `*[T]` lowers to `hop_slice_ro*`
- `*mut[T]` lowers to `hop_slice_mut*`

Notes:

- HopHop mutability semantics are language-level rules.
- C backend may encode those semantics with C `const` and/or codegen discipline.
- C field order is ABI-significant and must be stable per backend version.
- non-C backends may use different internal representation.

---

## Pointer arithmetic policy (proposal)

HEP-2 should not add general pointer arithmetic operators (`p + n`, `p - n`) initially.

Allowed ways to move/access memory:

- indexing (`a[i]`)
- slicing (`a[i:j]`)
- explicit helper intrinsics (future HEP if needed)

Rationale:

- keeps safe-mode bounds checks tractable
- reduces accidental UB surface
- keeps HopHop array/slice-first

---

## Conversion policy

Current conversion behavior in HEP-2 (explicit vs implicit):

```hop
*T         -> &T           // explicit
*T         -> mut&T        // explicit
*T       <-> *[T 1]        // explicit
*T         -> &[T 1]       // explicit
*T         -> mut&[T 1]    // explicit

mut&T      -> &T           // implicit
mut&[T N]  -> &[T N]       // implicit
mut[T]     -> [T]          // implicit

[T N]      -> mut[T]       // implicit
[T N]      -> [T]          // implicit
*[T N]     -> mut&[T N]    // implicit
*[T N]     -> &[T N]       // implicit
```

Reference assignment remains explicit:

- use `&expr` when assigning to `&T` / `mut&T`
- value-to-reference assignment is not implicit

Additional implicit coercions used by typecheck/codegen:

- `[T N] -> mut[T]` / `[T]` in initializers, assignment, call args, and returns
- `mut[T] -> [T]` in initializers, assignment, call args, and returns

Readonly-to-mutable conversions are not implicit.

---

## Examples

```hop
var v1 i32
var v2 [i32 3]
var p1 *[i32] = new(ma, i32, 3)

var r1 &i32 = &v1
*r1 = 2 // error: r1 is read-only

var r2 mut&i32 = &v1
*r2 = 2 // ok

var r3 [i32] = p1
r3[0] = 2 // error: r3 is read-only

var r4 mut[i32] = p1
r4[0] = 2 // ok

var r5 [i32] = v2[1:]
r5[0] = 2 // error: r5 is read-only

var r6 mut[i32] = v2[1:]
r6[0] = 2 // ok

var r7 mut[i32] = r5[:] // error: incompatible type mut[i32] <- [i32]
```

---

## Compatibility impact

HEP-2 intentionally changes syntax from:

- `[N]T` to `[T N]`
- `[.len]T` to `[T .len]`
- `const&...` read-only forms to read-only-by-default + `mut...` writable forms

Migration tooling should be considered once parser support is implemented.
