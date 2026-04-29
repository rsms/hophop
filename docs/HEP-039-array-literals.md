# HEP-39 array literals

Status: Draft

## Summary

HEP-39 adds array literal expressions:

```hop
var a          = [1, 2, 3]
var b [int 64] = [1, 2, 3]
var p          = alloc [1, 2, 3]
var q *[int]   = alloc [1, 2, 3]
```

An array literal is an array value. With no expected type, `[1, 2, 3]` infers to `[int 3]`.
Context can request a fixed-size array, a readonly reference/slice, or heap allocation through
`alloc`, but a bare array literal does not imply dynamic allocation.

## Motivation

HopHop already supports array types and indexing, but initializing small arrays currently requires
declaring storage and assigning each element separately. That is noisy for tests, lookup tables,
small fixed buffers, and values passed to APIs.

HEP-39 adds a compact literal form while keeping allocation and lifetime rules explicit:

- bare literals are by-value fixed arrays
- readonly reference/slice initialization requires const elements
- heap allocation uses `alloc`
- dynamic-size heap arrays are requested by expected type, not by the literal itself

## Goals

- Add `[expr, ...]` as expression syntax.
- Infer the length of a literal from its element count.
- Infer the element type from the literal elements using normal expression typing rules.
- Allow contextual typing to fixed-size arrays and readonly array/slice references.
- Allow `alloc [expr, ...]` for initialized heap arrays.
- Keep allocation policy explicit and deterministic.

## Non-goals

HEP-39 does not add:

- a `[T _]` type spelling
- growable array types
- hidden allocation for bare literals
- array literals with holes or index designators
- implicit mutable references to literal storage
- multi-dimensional index syntax such as `a[1, 2]`

## Syntax

Array literals are primary expressions:

```ebnf
PrimaryExpr      = ... | ArrayLit .
ArrayLit         = "[" [ ArrayElementList ] "]" .
ArrayElementList = Expr { ArrayElementSep Expr } [ ArrayElementSep ] .
ArrayElementSep  = "," | ";" .
AllocExpr          = "alloc" ( "[" Type Expr "]" | ArrayLit | Type [ "{" [ FieldInitList ] "}" ] )
                   [ "in" Expr ] .
```

`[` is not currently a primary-expression starter, so `[expr]` can be used for a single-element
array literal without conflicting with existing syntax.

Elements may be separated by commas or semicolons. This allows automatic semicolon insertion to make
multi-line literals compact:

```hop
x := [
    1
    2, 3
]
```

Examples:

```hop
var a = [1, 2, 3]
var b = [1]
var c = [1,]
var d = [1; 2; 3]
```

Invalid examples:

```hop
var a = [] // invalid without an expected element type
```

Empty literals are valid only with an expected array/reference/slice element type.

## Typing

### 1. No expected type

With no expected type, an array literal infers to a fixed-size array:

```hop
var a = [1, 2, 3] // [int 3]
var b = [1]       // [int 1]
```

The element type is inferred from all elements. Constant numeric literals follow the existing
defaulting rules. If no common element type can be inferred, the literal is rejected.

An empty literal has no element expressions, so it requires an expected type:

```hop
var a [int 0] = [] // ok
var b         = [] // invalid
```

### 2. Expected fixed-size array

When the expected type is `[T N]`, each provided element must be assignable to `T`.

If the literal has fewer than `N` elements, the remaining elements are initialized to zero/default
values using the same rules as ordinary array storage initialization.

```hop
var a [int 3]  = [1, 2, 3]
var b [int 64] = [1, 2, 3] // trailing elements are zero
```

If the literal has more than `N` elements, it is rejected.

### 3. Expected readonly array reference

When the expected type is `&[T N]`, the literal may initialize a readonly static array only if all
elements are const-evaluable:

```hop
var a &[int 3] = [1, 2, 3] // ok
```

The storage has static lifetime and is immutable. Binding a literal to a mutable reference is
invalid.

### 4. Expected readonly slice

When the expected type is `&[T]`, the same const-only rule applies. The compiler materializes a
readonly static `[T N]` and then applies the existing fixed-array-to-slice reference conversion:

```hop
var a &[int] = [1, 2, 3]
```

Non-constant elements are rejected because there is no runtime storage owner:

```hop
fn example(x int) {
    var a &[int] = [1, 2, x] // invalid
}
```

### 5. Runtime elements

Runtime elements are valid when the result is an owned fixed-size array value:

```hop
fn example(x int) {
    var a = [1, 2, x] // [int 3]
}
```

This keeps a small edit from changing the literal from value semantics to reference semantics.

## `alloc` literals

`alloc [expr, ...]` allocates initialized array storage.

With no expected type, the result is a pointer to a fixed-size array:

```hop
var p = alloc [1, 2, 3] // *[int 3]
```

When the expected type is `*[T]`, the result is a dynamic-size array with length equal to the
literal element count:

```hop
var q *[int] = alloc [1, 2, 3] // dynamic-size array with 3 elements
```

Each element must be assignable to `T`.

When the expected type is `*[T N]`, the literal initializes fixed-size heap storage. Missing
trailing elements are zero/default initialized, and excess elements are rejected:

```hop
var r *[int 64] = alloc [1, 2, 3]
```

The allocator behavior follows the active `alloc` rules. If HEP-38 is active, plain `alloc` uses GC
allocation and `alloc ... in allocator` uses explicit manual allocation.

## Constness

Array literal result type does not depend on whether elements are const-evaluable.

```hop
fn example(x int) {
    var a = [1, 2, 3] // [int 3]
    var b = [1, 2, x] // [int 3]
}
```

Constness only controls whether the compiler may materialize readonly static storage for expected
`&[T N]` and `&[T]` targets.

This intentionally differs from string literals. String literals are immutable byte data by nature;
array literals are general values and may be used as mutable local arrays.

## Conversions

HEP-39 relies on existing array and slice conversion rules where possible:

- `[T N]` can initialize `[T N]`
- `[T M]` can initialize `[T N]` only when `M <= N`; missing elements are default initialized
- const `[T N]` literal storage can bind to `&[T N]`
- `&[T N]` can convert to `&[T]` through existing fixed-array-to-slice reference behavior
- `alloc [expr, ...]` can produce `*[T]` only in an expected `*[T]` context
- `[]` can initialize a zero-length fixed array or dynamic-size heap array only with an expected
  element type

There is no implicit conversion from a bare runtime array literal to `&[T]`; such a conversion would
create a reference without an owner.

## Parser ambiguity

`[` is not currently a valid primary-expression starter, so `[expr, ...]` adds a alloc expression form
without conflicting with existing expression syntax.

Existing bracket uses remain context-specific:

- type positions: `[T]`, `[T N]`
- suffix positions: `a[i]`, `a[i:j]`
- declaration/type-name positions: type parameters and type arguments

`alloc [` needs parser disambiguation between existing `alloc [T n]` allocation syntax and added
`alloc [expr, ...]` literal allocation syntax. The same expression/type-context split already exists
elsewhere in the language and can be resolved by trying the existing type form first.

## Formatting

The formatter should preserve compact single-line comma-separated literals:

```hop
x := [1, 2, 3]
```

For multiline array literals, the formatter should use line breaks as separators where possible and
elide trailing commas that exist only to separate lines:

```hop
x := [
    1,
    2, 3,
]
```

Canonical formatted output:

```hop
x := [
    1
    2, 3
]
```

Commas may still be preserved within a line to group multiple elements on that line.

## Implementation notes

The implementation should add a distinct AST node for array literals rather than reusing compound
literals. Compound literals are field-name based and aggregate-oriented; array literals are ordered
element lists with contextual typing.

Suggested validation tests:

- parse and format `[1, 2, 3]`, `[1]`, `[1; 2; 3]`, multiline semicolon-inserted
  literals, multiline literals with trailing commas canonicalized away, and `[]`
- infer `var a = [1, 2, 3]` as `[int 3]`
- infer `var a = [1]` as `[int 1]`
- reject `var a = []`
- accept `var a [int 0] = []`
- infer `var a = [1, 2, x]` as `[int 3]`
- accept `var a [int 64] = [1, 2, 3]`
- reject excess elements for `[int N]`
- accept `var a &[int] = [1, 2, 3]`
- reject `var a &[int] = [1, 2, x]`
- accept `var p = alloc [1, 2, 3]`
- accept `var q *[int] = alloc [1, 2, 3]`
- reject mutable reference binding to literal static storage

## Open questions

1. Should explicit readonly static storage have a spelling separate from contextual `&[T]` targets?
2. Should array literals support later index designators, for example `[2: value]`, or should that
   remain out of scope permanently?
