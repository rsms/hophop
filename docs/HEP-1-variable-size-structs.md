# HEP-1 variable-size structs (completed)

## Summary

**HEP-1** adds **variable-size aggregates** to HopHop, centered on variable-size structs (VSS). A
variable-size aggregate has a runtime size that depends on field values (typically length fields),
while still honoring natural alignment.

Key features:

* No new `varsize` keyword. Variable-size status is inferred from fields.
* Dependent-length trailing array fields: `things [.thingsLen]Thing`.
* Variable-size status is transitive through aggregate fields:
  a struct/union containing a variable-size field is also variable-size.
* `sizeof(expr)` is runtime for variable-size aggregates.
* Generated C output target is **C11**.

---

## Motivation

For serialization and in-place parsed data, it is common to store:

* a fixed header,
* one or more length fields,
* then variable-length arrays immediately following in memory.

Example:

```hop
struct Packet {
  type u8
  version u8
  flags u16
  stuffLen u32
  thingsLen u32
  stuff  [.stuffLen]Stuff
  things [.thingsLen]Thing
}
```

Hand-written runtime size math is error-prone, mainly because alignment between trailing regions
must be preserved. HEP-1 makes these offsets and size calculations explicit and generated.

---

## Syntax

### Aggregate declarations

Struct/union declaration syntax is unchanged; there is no `varsize` modifier.

### Field declarations

Inside struct bodies, fields can be:

1. Fixed field

```hop
fieldName Type
```

2. Dependent-length trailing array field

```hop
fieldName [.lenFieldName]ElemType
```

Where:

* `lenFieldName` must name a previous fixed integer field in the same struct.
* `ElemType` must have known size/alignment.
* Dependent array fields must appear after all fixed fields.
* Multiple dependent array fields are allowed and are laid out sequentially in declaration order.

### `sizeof`

HopHop supports:

* `sizeof(Type)` — compile-time constant; invalid for variable-size aggregates.
* `sizeof(expr)` — runtime size in bytes (`uint`) for variable-size aggregates.

Examples:

```hop
sizeof(Packet)      // error: Packet is variable-size
sizeof(p)           // ok if p : *Packet
```

---

## Semantics

### Variable-size classification (normative)

An aggregate type is variable-size if:

* it declares at least one dependent-length trailing array field, or
* it contains a field whose type is variable-size.

This applies transitively to structs and unions.

### Core model

A variable-size struct instance describes one contiguous region:

* fixed header fields at the base,
* trailing variable-length regions computed from length fields.

A value of type `*Packet` points to the base of that region.

### Layout algorithm (normative)

Let:

* `base` be the address of an instance,
* `HdrSize` be the size of the fixed header by normal alignment rules,
* `align_up(x, a)` align `x` up to multiple of `a`, where `a` is a power of two.

For dependent arrays in declaration order:

1. `off = HdrSize`
2. For each field `f [.lenField]ElemType`:
   * `off = align_up(off, alignof(ElemType))`
   * `f_ptr = base + off`
   * `off += (lenField_value as uint) * sizeof(ElemType)` (wrap-around for now)
3. Total runtime size:
   * `size = align_up(off, alignof(StructHeaderType))` (mandatory final alignment)

Overflow behavior is currently wrap-around arithmetic. Overflow checking can be added later via
UBSan or compiler intrinsics.

### Field access

For `p : *Packet`:

* Fixed fields behave normally: `p.flags`, `p.stuffLen`.
* Dependent array fields are computed pointer-valued properties:
  * `p.stuff : *Stuff`
  * `p.things : *Thing`
* Indexing works: `p.stuff[i]`.
* Assignment is illegal: `p.stuff = x` is a compile error.
* Taking address is allowed: `&p.stuff` (footgun, same class as C footguns, accepted for now).

### Type restrictions

* `lenFieldName` must reference an earlier fixed integer field (`u8..u64`, `i8..i64`, `uint`,
  `int`).
* `lenField_value` is coerced to `uint` for size math.
* By-value variable declarations of variable-size aggregate type are errors:
  * `var x Packet` is illegal
  * `var x *Packet` is legal

---

## Code generation (C11)

### Representation strategy

C cannot directly model multiple aligned trailing dynamic regions as native fields, so generated C
uses:

1. Fixed header struct containing only fixed fields.
2. Inline accessor functions for dependent fields.
3. Inline runtime size function used for `sizeof(expr)` lowering.

### Generated C template (illustrative)

For:

```hop
struct Packet {
  type u8
  version u8
  flags u16
  stuffLen u32
  thingsLen u32
  stuff  [.stuffLen]Stuff
  things [.thingsLen]Thing
}
```

Emit header struct:

```c
typedef struct Packet__hdr {
  u8  type;
  u8  version;
  u16 flags;
  u32 stuffLen;
  u32 thingsLen;
} Packet__hdr;

typedef Packet__hdr Packet;
```

Emit helper:

```c
// a must be power-of-two
static inline __hop_uint hop_align_up(__hop_uint x, __hop_uint a) { return (x + (a - 1)) & ~(a - 1); }
```

Emit accessors:

```c
static inline Stuff* Packet__stuff(Packet* p) {
  __hop_uint off = sizeof(Packet__hdr);
  off = hop_align_up(off, _Alignof(Stuff));
  return (Stuff*)((__hop_u8*)p + off);
}

static inline Thing* Packet__things(Packet* p) {
  __hop_uint off = sizeof(Packet__hdr);
  off = hop_align_up(off, _Alignof(Stuff));
  off += (__hop_uint)p->stuffLen * sizeof(Stuff);
  off = hop_align_up(off, _Alignof(Thing));
  return (Thing*)((__hop_u8*)p + off);
}
```

Emit runtime size:

```c
static inline __hop_uint Packet__sizeof(Packet* p) {
  __hop_uint off = sizeof(Packet__hdr);

  off = hop_align_up(off, _Alignof(Stuff));
  off += (__hop_uint)p->stuffLen * sizeof(Stuff);

  off = hop_align_up(off, _Alignof(Thing));
  off += (__hop_uint)p->thingsLen * sizeof(Thing);

  off = hop_align_up(off, _Alignof(Packet__hdr)); // mandatory final alignment
  return off;
}
```

### HopHop lowering rules (normative)

* `p.stuff` -> `Packet__stuff(p)`
* `p.things` -> `Packet__things(p)`
* `sizeof(p)` where `p : *Packet` and `Packet` is variable-size -> `Packet__sizeof(p)`
* `sizeof(Packet)` -> compile error

Regular fixed fields keep standard lowering (`.` vs `->` as usual).

### Backend API surface

Accessor/helper names are codegen-backend details, not language-level API guarantees. A different
backend (e.g. Lua/JS/WASM) may represent variable-size fields differently.

---

## Language and parser changes

### Grammar extensions (EBNF-level)

1. No type declaration modifier changes are required (`struct`/`union` syntax stays the same).
2. Field extension in struct bodies:
   * `FieldDecl ::= Ident ( VarArrayType | Type )`
   * `VarArrayType ::= "[" "." Ident "]" Type`
3. `sizeof` form support:
   * `sizeof(Type)` (compile-time form)
   * `sizeof(Expr)` (runtime form)
   * semantic restriction: type-form illegal for variable-size aggregate types.

---

## Typechecking requirements

* Validate dependent field ordering and references:
  * dependent fields appear after fixed fields
  * each dependent field references an earlier fixed integer field
* Compute variable-size classification transitively for struct/union aggregates.
* Record fixed-header + dependent-field metadata for codegen.
* Type dependent fields as computed `*ElemType` properties.
* Reject assignment to dependent fields (`p.stuff = ...`).
* Reject by-value variable declarations for variable-size aggregate types.
* Type `sizeof(expr)` as `uint`; route variable-size cases to runtime codegen helper.

---

## Implementation notes

* Keep layout math in one shared routine used by accessors + runtime size helpers.
* Use C11 `_Alignof(T)` and `sizeof(T)` in generated C.
* Overflow is wrap-around for now; overflow diagnostics may be added later via UBSan/intrinsics.
* Emit one reusable `hop_align_up` helper in prelude/runtime support.

---

## Example usage (HopHop)

```hop
fn handle(p *Packet) void {
  var total uint = sizeof(p)

  var s *Stuff = p.stuff
  var t *Thing = p.things

  assert total > 0 as uint
}
```

---

## Open questions (optional future work)

* Support offset-based fields (e.g. `things [@.thingsOff : .thingsLen]Thing`).
* Add `len(p.stuff)` sugar mapped to the associated length field.
* Add safe construction patterns for backing buffers (e.g. `buf[...].as(Packet)`).

---

## Status

**Proposed.** Ready to integrate into the core HopHop spec and code generator as a v1 feature increment
(HEP-1).
