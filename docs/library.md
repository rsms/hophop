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
[T]        // read-only slice view (a form of reference)
mut[T]     // mutable slice view (a form of reference)

*T         // pointer to value
*[T N]     // pointer to fixed-size array
*[T]       // pointer to runtime-length array

&T         // read-only reference to value
mut&T      // mutable reference to value
&[T N]     // read-only reference to fixed-size array
mut&[T N]  // mutable reference to fixed-size array
```

Notes:
- `[T N]` is a value type; `[T]`/`mut[T]` are view types.
- `mut` changes mutability of a view/reference.
- Common implicit conversions include:
  - `[T N] -> [T]`
  - `[T N] -> mut[T]`
  - `mut[T] -> [T]`
  - `mut&[T N] -> &[T N]`

## Built-In Types

### `str`

`str` is a specialized string type.
Conceptually, it is a specialized form of `[u8]` where the content is UTF-8 data.

Properties:
- `len(s)` is byte length.
- `cstr(s)` exposes a read-only reference to UTF-8 bytes for C interop.
- Use `[u8]` / `mut[u8]` for arbitrary binary data.

### `__sl_MemAllocator`

`__sl_MemAllocator` is the low-level allocator capability used by `new(...)`.
Allocator implementations must zero newly allocated bytes:
- fresh allocations are fully zeroed
- resized allocations must zero bytes in `[oldSize, newSize)`

For normal code, use `std/mem.Allocator` (a nominal alias of `__sl_MemAllocator`).

## Built-In Functions


### `len`

```sl
fn len(x Seq) u32
```

`len` returns the logical length of a sequence.
`Seq` can be an array or slice type (including `str`) as a value, reference, or pointer.


### `cstr`

```sl
fn cstr(s str) &u8
```

`cstr` returns a read-only reference to null-terminated UTF-8 bytes suitable for C APIs.


### `new`

```sl
fn new(ma mut&__sl_MemAllocator, type T) ?*T             // 1
fn new(ma mut&__sl_MemAllocator, type T) *T              // 2
fn new(ma mut&__sl_MemAllocator, type T, N uint) ?*[T N] // 3
fn new(ma mut&__sl_MemAllocator, type T, N uint) *[T N]  // 4
fn new(ma mut&__sl_MemAllocator, type T, N uint) ?*[T]   // 5
fn new(ma mut&__sl_MemAllocator, type T, N uint) *[T]    // 6
```

`new` allocates memory from a memory allocator.

- `ma` must be convertible to `mut&__sl_MemAllocator`.
- `T` is the allocated element type.
- If `N` is provided, storage for a sequence of `T` is allocated.
- If `N` is a positive compile-time constant, result type is a fixed-size array pointer (`*[T N]` / `?*[T N]`).
- Otherwise, result type is a runtime-length sequence pointer (`*[T]` / `?*[T]`).
- `T` cannot be a variable-size-by-value type.
- Negative compile-time `N` is rejected.
- `?*...` forms return null on allocation failure.
- `*...` forms panic on allocation failure.
- On success, newly allocated bytes are zero-initialized.


### `panic`

```sl
fn panic(message str)
```

`panic` logs an error message and stops program execution.
Exact behavior is platform-specific; it's implemented by `platform.panic`.


### `print`

```sl
fn print(message str)
```

`print` writes a UTF-8 message to standard output.
It is implemented via `platform.console_log(message, 0)`.


### `sizeof`

```sl
fn sizeof(type T) uint
fn sizeof(expr E) uint
```

`sizeof` returns the size in bytes needed to represent a type or expression.

- `sizeof(type T)` is compile-time.
- `sizeof(type T)` is invalid for variable-sized-by-value types.
- `sizeof(expr E)` is compile-time for fixed-size types and runtime for variable-sized values.


## Platform Package API

The platform package is the host boundary for panic/logging/exit operations.

Import path:
- `import "platform"`

Surface API:
- `platform.exit(status)`
- `platform.panic(msg, flags)`
- `platform.console_log(msg, flags)`

Operation semantics:
- `exit`: terminate process with status code.
- `panic`: handles panic and does not return.
- `console_log`: writes text (`flags=0` stdout, `flags=1` stderr).

Allocation is provided by `std/mem.Allocator` via `new(...)`, with the platform setting
`mem.platformAllocator` before `sl_main`.

Concrete default platform implementation used by `slc compile`/`slc run`:
- `lib/platform_libc.c`

## C Interop Mapping

Common SL <-> C type correspondences:

- `&T` <-> `const T*`
- `mut&T` <-> `T*`
- `*T` <-> `T*`
- `*[T N]` <-> `T*`
- `*[T]` <-> `struct { T* ptr; size_t len; }`
- `&[T N]` <-> `const T*`
- `mut&[T N]` <-> `T*`
- `&[T]` <-> `struct { const T* ptr; size_t len; }`
- `mut&[T]` <-> `struct { T* ptr; size_t len; }`
