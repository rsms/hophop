# SL Library Reference

This document describes the built-in library surface of SL:
- built-in non-primitive types
- commonly used type forms (arrays, slices, pointers, references)
- built-in functions
- platform package API

Primitive scalar types and full language grammar are documented in `docs/language.md`.

## Type Forms (Arrays and Slices)

```sl
T          // value
[T N]      // fixed-size array value
[T]        // unsized slice type (not by-value)

*T         // writable view of value
&T         // read-only view of value

*[T N]     // pointer to fixed-size array
&[T N]     // read-only view of fixed-size array
*[T]       // pointer to runtime-length array
&[T]       // read-only runtime-length slice view
```

Notes:
- `[T N]` is a value type.
- `[T]` is unsized and only valid behind `*` or `&`.
- Mutability is encoded by `*` (writable) vs `&` (read-only).
- Common implicit conversions include:
  - `[T N] -> &[T]`
  - `[T N] -> *[T]` when source is mutable lvalue
  - `&[T N] -> &[T]`
  - `*[T N] -> *[T]`
  - `*[T] -> &[T]`
  - `*T -> &T`

## Built-In Types

### `str`

`str` is a specialized string type.
Conceptually, it is a specialized form of `[u8]` where the content is UTF-8 data.

Properties:
- `len(s)` is byte length.
- `cstr(s)` exposes a read-only reference to UTF-8 bytes for C interop.
- Use `&[u8]` / `*[u8]` for arbitrary binary data.

### `__sl_MemAllocator`

`__sl_MemAllocator` is the low-level allocator capability used by `new(...)`.
Allocator implementations must zero newly allocated bytes:
- fresh allocations are fully zeroed
- resized allocations must zero bytes in `[oldSize, newSize)`

For normal code, use `Allocator` (a nominal alias of `__sl_MemAllocator`).

## Built-In Functions


### `len`

```sl
fn len(x Seq) u32
```

`len` returns the logical length of a sequence.
`Seq` can be an array or slice type (including `str`) as a value, reference, or pointer.


### `cstr`

```sl
fn cstr(s str) *u8
```

`cstr` returns a read-only reference to null-terminated UTF-8 bytes suitable for C APIs.


### `new`

```sl
fn new(ma *Allocator, type T) *T
fn new(ma *Allocator, type T, N uint) *[T N] // compile-time constant positive N
fn new(ma *Allocator, type T, N uint) *[T]   // otherwise

fn new(type T) *T
fn new(type T, N uint) *[T N] // compile-time constant positive N
fn new(type T, N uint) *[T]   // otherwise
```

`new` allocates memory from a memory allocator.

- `ma` must be convertible to `*Allocator`.
- Contextual forms (`new(T[, N])`) use allocator capability `mem` from effective context.
- Effective context `mem` must be assignable to `*Allocator`.
- `T` is the allocated element type.
- If `N` is provided, storage for a sequence of `T` is allocated.
- If `N` is a positive compile-time constant, result type is a fixed-size array pointer (`*[T N]`).
- Otherwise, result type is a runtime-length sequence pointer (`*[T]`).
- `T` cannot be a variable-size-by-value type.
- Negative compile-time `N` is rejected.
- `new` can still be assigned to optional pointer forms (`?*...`) via regular assignability.
- Panic-on-failure vs nullable behavior is determined by assignment target and lowering rules.
- On success, newly allocated bytes are zero-initialized.


### `panic`

```sl
fn panic(message str)
```

`panic` logs an error message and stops program execution.
Exact behavior is platform-specific; it lowers to the platform panic operation.


### `print`

```sl
fn print(message str)
```

`print` writes a UTF-8 message to standard output.
It is implemented by calling `context.log.handler(&context.log, message, LogLevel.Info, 0)`.


### `sizeof`

```sl
fn sizeof(type T) uint
fn sizeof(expr E) uint
```

`sizeof` returns the size in bytes needed to represent a type or expression.

- `sizeof(type T)` is compile-time.
- `sizeof(type T)` is invalid for unsized types (`[T]`) and variable-sized-by-value types.
- `sizeof(expr E)` is compile-time for fixed-size values and runtime for:
  - `*[T]` / `&[T]` as `len(E) * sizeof(type T)`
  - `*V` / `&V` where `V` is variable-size aggregate


## Platform Package API

The platform package is the host boundary for panic/logging/exit operations.

Import path:
- `import "platform"`

Surface API:
- `platform.exit(status)`

Operation semantics:
- `exit`: terminate process with status code.

Allocation is provided by `Allocator` via `new(...)`, with the platform setting
`context.mem` before `sl_main`.

Concrete default platform implementation used by `slc compile`/`slc run`:
- `lib/platform/cli-libc/platform.c`

### Draft delta (SLP-17, not implemented)

Planned additions to platform library surface:

- Base context in built-in `platform` package:

```sl
pub struct Context {
    mem     *Allocator
    console i32
    stdin   ?i32
    fs      ?__sl_FileSystem
}
```

- Target context packages under `platform/<target>` that compose the base:

```sl
import "platform"

pub struct Context {
    platform.Context
    // target-specific fields
}
```

This keeps a stable portable base (`platform.Context`) while allowing target extension.
`main` is planned to receive the selected target context type.
`__sl_FileSystem` is planned as a built-in type initially; future work may define
global host types in `prelude.sl` instead.

## Reflection Package (Draft)

`std/reflection` is proposed in SLP-18 as a compile-time reflection API.

Planned surface (draft):

- `reflection.Kind` enum for type categories (`Primitive`, `Alias`, `Struct`, etc.)
- Type reflection operations via `typeof` and type-value methods:
  - `.kind()`
  - `.base()` (for aliases)
  - `.fields()` (for aggregates)

Sketch examples:

```sl
import "std/reflection"

type MyInt int
struct Foo { x, y int }

fn main() {
    var x i32
    assert typeof(x) == i32
    assert i32.kind() == reflection.Kind.Primitive
    assert MyInt.kind() == reflection.Kind.Alias
    assert MyInt.base() == int
    assert Foo.kind() == reflection.Kind.Struct
    assert Foo.fields().len() == 2
}
```

Exact signatures and typing rules are draft and defined in `docs/SLP-18-reflection.md`.

## C Interop Mapping

Common SL <-> C type correspondences:

- `&T` <-> `const T*`
- `*T` <-> `T*`
- `*[T N]` <-> `T*`
- `*[T]` <-> `struct { T* ptr; size_t len; }`
- `&[T N]` <-> `const T*`
- `&[T]` <-> `struct { const T* ptr; size_t len; }`
