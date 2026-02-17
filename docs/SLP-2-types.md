# SLP-2 types, pointers, references and slices

## Summary

SLP-2 introduces:

- updated array type syntax (`[N T]` and `[.len T]`)
- pointer, reference, and slice type forms
- explicit mutability via `const&...`
- slice expressions with compile-time/runtime bounds checks
- `new` allocation forms for single values and arrays

For now, this is intentionally C-like:

- `*` and `&` may both be null
- no Rust-style lifetime/escape enforcement
- ownership meaning is semantic/conventional (`*` = owned, `&` = borrowed)

---

## Type forms

```sl
// Value types
T        // one value
[N T]    // array of N values
[.len T] // dependent-length array type form (used where applicable)

// Pointer types (owned)
*T       // pointer to one value
*[T]     // pointer to runtime-length array (ptr + len)
*[N T]   // pointer to fixed-length array

// Reference types (borrowed)
&T          // reference to one value
&[N T]      // reference to fixed-length array
&[T]        // mutable slice (ptr + len + cap)
const&T     // read-only reference to one value
const&[N T] // read-only reference to fixed-length array
const&[T]   // read-only slice (ptr + len)
```

### Nullability (current vs future)

- Current: `*` and `&` values may be null.
- Future SLP: add `?` optional-value syntax:
  - `?*T` may be null
  - `*T` non-null

---

## Semantics of `*` vs `&`

`*` and `&` are concretely the same low-level concept (addressable memory reference), but with
different semantic meaning:

- `*` means memory managed by me (owned)
- `&` means memory managed elsewhere (borrowed)

The compiler does not attempt Rust-like lifetime/escape checks in SLP-2.

---

## Address-of and addressability

`&expr` is valid for expressions that:

1. already reside in memory, or
2. can be trivially moved to memory (e.g. literal values).

This includes function arguments such as:

```sl
fn f(x &int) void
f(&3) // materialize temporary storage for 3 and pass its address
```

This follows C-style responsibility and footgun profile.

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
  - `a[:]`   = `a[0:len(a)]`
  - `a[n:]`  = `a[n:len(a)]` (result length is `len(a) - n`)
  - `a[:m]`  = `a[0:m]`

### Bounds checks

- If `start` and `end` are compile-time constants: check at compile time.
- Otherwise: emit runtime checks.
- Out-of-bounds access panics.

### Safe mode

- `slc` has a safe mode, on by default:
  - inserts bounds checks for memory accesses that may go out of bounds
  - compiles generated C with UBSan when available (for overflow/alignment traps)
- `slc --unsafe` disables safe-mode inserts.

---

## Mutability model

### `const` placement

- `const&T` means `T` is immutable (like `const T*` in C).
- `const*T` is invalid.
- `const T` is invalid in SLP-2.

Potential future extension: field-level `const` values (`timestamp const u64`) can be explored in a
separate SLP.

### Arrays/slices and const

- `const&[N T]` and `const&[T]` mean:
  - array/slice view metadata is immutable
  - elements are immutable

Such values may be stored in read-only memory (`.rodata`) when applicable.

---

## Equality and ordering

No built-in comparison support is added in SLP-2.

Ordering/comparison for arrays/slices/pointers is deferred to a separate SLP.

---

## Default / zero value behavior

- Pointer/reference zero value is null.
- `len` on null pointer/reference to array/slice returns 0.

Future with `?` nullability:

- Non-null reference fields (e.g. `r &i32`) in by-value aggregates must be initialized before use.
- `var s S` or `var s S = {}` may be rejected unless initialization is proven.
- Pointer-allocated values (`new`) are an exception and may start with null fields that must be
  assigned by the programmer.

Static analysis for definite initialization can be added later.

---

## Allocation API (`new`)

Logical signatures (current):

```sl
fn new(ma &MemAllocator, type T) *T
fn new(ma &MemAllocator, type T, n usize) *[n T] // logical dependent return shape
```

Logical signatures (future with `?`):

```sl
fn new(ma &MemAllocator, type T) ?*T
fn new(ma &MemAllocator, type T, n usize) ?*[n T]

// non-null forms that panic on allocation failure
fn new(ma &MemAllocator, type T) *T
fn new(ma &MemAllocator, type T, n usize) *[n T]
```

Ownership:

- caller owns result and must free when done.

---

## `str` in SLP-2

Type aliases are a separate SLP and out of scope here.

For SLP-2, treat `str` as:

- `str`  = `const&[u8]`
- `*str` = `*[u8]`
- `&str` = `&[u8]`

Design intent is to keep strings as UTF-8 byte-array views rather than splitting by storage class.

---

## Canonical type grammar and precedence

### EBNF

```ebnf
Type            = QualRefType
                | RefType
                | PtrType
                | ArrayType
                | DynArrayType
                | DepArrayType
                | TypeName ;

QualRefType     = "const" "&" Type ;
RefType         = "&" Type ;
PtrType         = "*" Type ;

ArrayType       = "[" ConstExpr Type "]" ;
DynArrayType    = "[" Type "]" ;
DepArrayType    = "[" "." Identifier Type "]" ;

TypeName        = Identifier { "." Identifier } ;
```

### Precedence/association

- Type constructors are right-associative:
  - `*&T` parses as `*( &T )`
  - `&*T` parses as `&( *T )`
- `const` in SLP-2 is only valid in `const&...`.
- `const T` and `const*T` are invalid.

---

## Representation and ABI mapping (C backend proposal)

The following is the C backend mapping target for SLP-2:

```c
// *[T]
struct { void* ptr; size_t len; };

// &[T]
struct { void* const ptr; size_t len; const size_t cap; };

// const&[T]
struct { const void* const ptr; const size_t len; };
```

Fixed-array refs:

- `&[N T]` maps to `T*` plus compile-time length `N` in type semantics.
- `const&[N T]` maps to `const T*` plus compile-time length `N`.
- `*[N T]` maps to `T*` plus compile-time length `N` (owned).

Notes:

- SL mutability semantics are language-level rules.
- C backend may encode the same semantics either via C `const` fields or by codegen discipline.
- SL mutability rules are enforced by SL typechecking.
- C field order is ABI-significant and must be stable per backend version.
- Backends other than C may choose different internal representation.

---

## Pointer arithmetic policy (proposal)

SLP-2 should not add general pointer arithmetic operators (`p + n`, `p - n`) initially.

Allowed ways to move/access memory:

- indexing (`a[i]`)
- slicing (`a[i:j]`)
- explicit helper intrinsics (future SLP if needed)

Rationale:

- keeps safe-mode bounds checks tractable
- reduces C-style accidental UB surface
- matches SL goal of clear array/slice-first data access

---

## Conversion policy

Current conversion intent for SLP-2 (explicit vs implicit):

```sl
*T      -> &T           // explicit
*T      -> const&T      // explicit
*T    <-> *[1 T]        // explicit
*T      -> &[1 T]       // explicit
*T      -> const&[1 T]  // explicit

&T      -> const&T      // implicit
&T    <-> &[1 T]        // implicit
&T      -> const&[1 T]  // implicit

[N T]   -> &[N T]       // implicit
[N T]   -> &[T]         // implicit
[N T]   -> const&[N T]  // implicit
[N T]   -> const&[T]    // implicit

&[N T]  -> &[T]         // implicit
&[N T]  -> const&[T]    // implicit
*[N T]  -> &[N T]       // implicit
*[N T]  -> const&[N T]  // implicit
&[T]    -> const&[T]    // implicit
```

---

## Compatibility impact

SLP-2 intentionally changes syntax from:

- `[N]T` to `[N T]`
- `[.len]T` to `[.len T]`
- declaration-form-only const usage to type-form `const&...` for read-only references

Migration tooling should be considered once parser support is implemented.
