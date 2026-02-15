# SLP-1 variable-size structs

## Summary

**SLP-1** adds **variable-size structs** (VSS) to SL: structs whose total size depends on runtime values (typically length fields) while still honoring **natural alignment** (unlike “packed” layouts). VSS are aimed at **serialized / in-place parsed data** with one or more trailing variable-length arrays, common in file formats, network packets, and embedded binary blobs.

Key features:

* A new `varsize struct` modifier.
* Dependent-length trailing array fields: `things [.thingsLen]Thing`.
* Natural alignment for all fields and trailing arrays.
* `sizeof(expr)` becomes a **runtime** operation for VSS instances, avoiding manual size math and alignment bugs.
* VSS are represented in generated C99 using a fixed header struct + inline accessors and runtime size computation.

---

## Motivation

For serialization and packed data, it’s common to store:

* a fixed header,
* one or more length fields,
* then variable-length arrays that immediately follow in memory.

Example:

```sl
varsize struct Packet {
  type u8
  version u8
  flags u16
  stuffLen u32
  thingsLen u32
  stuff  [.stuffLen]Stuff
  things [.thingsLen]Thing
}
```

Hand-calculating runtime size is error-prone because you must account for **alignment between arrays**, e.g. `Thing` might have 16-byte alignment. SLP-1 solves this by making `sizeof(p)` compute the correct runtime size using the compiler’s layout rules.

---

## Syntax

### Variable-size struct declaration

```sl
varsize struct Name { FieldDecl* }
```

### Field declarations

Inside a `varsize struct` body, fields can be:

1. **Fixed field**

```sl
fieldName Type
```

2. **Dependent-length trailing array field**

```sl
fieldName [.lenFieldName]ElemType
```

Where:

* `lenFieldName` must name a **previous** fixed integer field in the same struct.
* `ElemType` is any type with a known element size and alignment.
* Dependent array fields must appear **after all fixed fields**.
* Multiple dependent array fields are allowed and are laid out **sequentially** in declaration order.

### `sizeof`

SL supports two forms:

* `sizeof(Type)` — compile-time constant; **invalid for varsize structs**
* `sizeof(expr)` — runtime size in bytes (returns `usize`); **required for varsize structs**

Examples:

```sl
sizeof(Packet)      // ERROR: Packet is varsize
sizeof(p)           // OK if p : *Packet (or Packet by-value if allowed)
```

---

## Semantics

### Core model

A `varsize struct` describes a **contiguous memory region**:

* A fixed “header” containing all fixed fields.
* Followed by one or more variable arrays, each aligned to its element type.

A value of type `*Packet` is a pointer to the beginning of that memory region.

### Layout algorithm (normative)

Let:

* `base` be the address of an instance,
* `HdrSize` be the size of the fixed header per normal alignment rules,
* `align_up(x, a)` align `x` up to multiple of `a`.

For dependent arrays in declaration order:

1. Start offset:

   * `off = HdrSize`

2. For each dependent array field `f [.lenField]ElemType`:

   * `off = align_up(off, alignof(ElemType))`
   * `f_ptr = base + off`
   * `off += (lenField_value as usize) * sizeof(ElemType)`

3. Total runtime size:

   * `size = align_up(off, alignof(StructHeaderType))` (optional final alignment; recommended)

### Field access

For `p : *Packet`:

* Fixed fields behave like normal struct fields: `p.flags`, `p.stuffLen`.
* Dependent array fields evaluate to a pointer to element type:

  * `p.stuff` has type `*Stuff`
  * `p.things` has type `*Thing`
* Indexing works normally:

  * `p.stuff[i]` is valid.

### Type restrictions

* `lenFieldName` must refer to an earlier fixed field whose type is an integer type (`u8..u64`, `i8..i64`, `usize`, `isize`).
* The compiler coerces `lenField_value` to `usize` for size computation; representability checks follow existing SL rules.

### Prohibited / constrained operations

To prevent misleading “normal struct” assumptions:

* `sizeof(Type)` is disallowed for varsize structs.
* Taking addresses of dependent fields like `&p.stuff` is allowed (it’s a pointer value), but treating `p.stuff` as a stored header field is not meaningful; it is a computed pointer.
* Optional policy (recommended): disallow by-value variables of varsize struct types (`var x Packet`) unless backed by a fixed-size buffer mechanism. v0 can simply require varsize structs be used behind pointers.

---

## Code generation (C99)

### Representation strategy

C cannot directly express multiple flexible trailing arrays with alignment constraints. Therefore, generated C uses:

1. A fixed header struct containing only the fixed fields.
2. Inline accessor functions for each dependent array field.
3. An inline function for runtime size (`sizeof(p)` lowering).

### Generated C template (illustrative)

For:

```sl
varsize struct Packet {
  type u8
  version u8
  flags u16
  stuffLen u32
  thingsLen u32
  stuff  [.stuffLen]Stuff
  things [.thingsLen]Thing
}
```

Emit a header struct:

```c
typedef struct Packet__hdr {
  u8  type;
  u8  version;
  u16 flags;
  u32 stuffLen;
  u32 thingsLen;
} Packet__hdr;

typedef Packet__hdr Packet; // SL name "Packet" refers to header at base
```

Emit helpers (alignment helper may live in prelude):

```c
static inline usize sl_align_up(usize x, usize a) { return (x + (a-1)) & ~(a-1); }
```

Emit accessors:

```c
static inline Stuff* Packet__stuff(Packet* p) {
  usize off = sizeof(Packet__hdr);
  off = sl_align_up(off, _Alignof(Stuff));
  return (Stuff*)((u8*)p + off);
}

static inline Thing* Packet__things(Packet* p) {
  usize off = sizeof(Packet__hdr);
  off = sl_align_up(off, _Alignof(Stuff));
  off += (usize)p->stuffLen * sizeof(Stuff);
  off = sl_align_up(off, _Alignof(Thing));
  return (Thing*)((u8*)p + off);
}
```

Emit runtime size:

```c
static inline usize Packet__sizeof(Packet* p) {
  usize off = sizeof(Packet__hdr);

  off = sl_align_up(off, _Alignof(Stuff));
  off += (usize)p->stuffLen * sizeof(Stuff);

  off = sl_align_up(off, _Alignof(Thing));
  off += (usize)p->thingsLen * sizeof(Thing);

  off = sl_align_up(off, _Alignof(Packet__hdr)); // recommended final align
  return off;
}
```

### SL lowering rules (normative)

* `p.stuff` → `Packet__stuff(p)`
* `p.things` → `Packet__things(p)`
* `sizeof(p)` where `p : *Packet` and `Packet` is varsize → `Packet__sizeof(p)`
* `sizeof(Packet)` → compile error

Field access `p.flags` still lowers using `. vs ->` rule (`p->flags` since `p` is pointer).

---

## Language and parser changes

### Grammar extensions (EBNF-level)

1. Type declaration modifier:

* Allow optional `varsize` before `struct`:

  * `TypeDecl ::= ("varsize")? "struct" Ident StructBody`

2. Field declaration extension for dependent arrays:

* In struct bodies:

  * `FieldDecl ::= Ident ( VarArrayType | Type )`
  * `VarArrayType ::= "[" "." Ident "]" Type`

3. `sizeof` expression form (if not already present):

* Support both:

  * `sizeof(Type)` (compile-time)
  * `sizeof(Expr)` (runtime)
* Semantic restriction for varsize: type-form illegal.

---

## Typechecking requirements

* Validate ordering:

  * All dependent array fields must appear after all fixed fields.
  * Each dependent array must reference an earlier fixed integer field.
* Compute and store, per varsize struct:

  * fixed header layout info (field offsets/sizes/alignments),
  * list of dependent arrays with their element type and length field.
* Mark dependent array “fields” as computed properties returning `*ElemType`.
* Implement `sizeof(expr)` typing:

  * returns `usize`
  * if expr is `*VarSizeStruct`, route to runtime size logic in codegen.

---

## Implementation notes

* Keep varsize layout computation in a single shared routine so:

  * accessors and `sizeof` share logic,
  * you can const-fold where possible.
* Always use `_Alignof(T)` / `sizeof(T)` in C99 output.
* Prefer generating a small `sl_align_up` helper in the prelude once, reused everywhere.

---

## Example usage (SL)

```sl
fun handle(p *Packet) void {
  // safe: runtime size accounts for alignment between stuff and things
  var total usize = sizeof(p)

  // access variable arrays
  var s *Stuff = p.stuff
  var t *Thing = p.things

  assert p.stuffLen >= 0 as u32
  assert total > 0 as usize
}
```

---

## Open questions (optional future work)

* Support offset-based fields (e.g. `things [@.thingsOff : .thingsLen]Thing`) for more complex formats.
* Allow `len(p.stuff)` sugar to map to the corresponding length field (requires recording association).
* Allow varsize structs to be safely constructed with a backing buffer type (e.g. `buf[...].as(Packet)` patterns).

---

## Status

**Proposed.** Ready to integrate into the core SL spec and code generator as a v1 feature increment (SLP-1).
