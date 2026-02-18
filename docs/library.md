# SL Library Reference

This document describes the built-in library surface of SL:
- built-in non-primitive types
- built-in functions
- platform package API

Primitive scalar types and language syntax are documented in `docs/language.md`.

## Built-In types

### `str`

`str` is a specialized string type. Conceptually, it is a specialized form of `[u8]` where
the data is UTF-8 text.

Properties:
- `len(s)` is byte length.
- `cstr(s)` exposes a pointer to UTF-8 bytes for C interop.
- Use `[u8]`/`mut[u8]` for arbitrary binary data.


### `MemAllocator`

`MemAllocator` is the allocator capability used by [`new`](#new)


## Built-In functions


### `len`

```sl
fn len(x Seq) uint
```

`len` returns the logical length of a sequence.
`Seq` is an array or slice type (including `str`). It may be a value, reference, or pointer to such a type.


### `cstr`

```sl
fn cstr(s str) &u8
```

`cstr` returns a reference to a null-terminated string of bytes, suitable for use with C APIs.


### `new`

```sl
fn new(ma mut&MemAllocator, type T) ?*T             // 1
fn new(ma mut&MemAllocator, type T) *T              // 2
fn new(ma mut&MemAllocator, type T, N uint) ?*[T N] // 3
fn new(ma mut&MemAllocator, type T, N uint) *[T N]  // 4
fn new(ma mut&MemAllocator, type T, N uint) ?*[T]   // 5
fn new(ma mut&MemAllocator, type T, N uint) *[T]    // 6
```

`new` allocates memory from a memory allocator.
- `T` is the type for which to allocate memory for. Size and alignment is derived from this.
- If `N` is given, an array is allocated.
    - If `N` is a compile-time constant, the result's type is a comptime-size array. (Forms 3 and 4.)
    - If `N` is _not_ a compile-time constant, the result's type is a runtime-size array. (Forms 3 and 4)
    - Note that a comptime-size array is convertible to a runtime-size array, so you can use either type as the receiver's type. I.e. `*[T N]` is convertible to `*[T]`.
- If the receiver's type is an optional (`?*…`), the allocator returns null if allocation fails (form 1, 3 and 5.) Otherwise the allocator will panic if allocation fails (form 2, 4 and 6.)


### `panic`

```sl
fn panic(message str)
```

`panic` log an error `message` and then stops execution of the program.
The exact behavior is [platform](#platform) specific.


### `sizeof(Type)` / `sizeof(expr) -> uint`

```sl
fn sizeof(type T) uint // 1
fn sizeof(expr E) uint // 2
```

`sizeof` returns the size in bytes which a type or expression needs to be completely represented.

- Form 1 is guaranteed to be a compile-time value.
- Form 1 is not available for variable-sized types, like variable-size structs.
- Form 2 is a compile-time value for fixed-size types, runtime value for variable-sized types.


## Platform

The platform package is the host boundary for panic, logging, and memory operations.

Import path:
- `import "platform"`

Surface API:
- `platform.panic(msg, flags)`
- `platform.console_log(msg, flags)`
- `platform.alloc(size, align, flags)`
- `platform.resize(ptr, oldSize, newSize, align, flags)`
- `platform.free(ptr, size, flags)`

Operation semantics:
- `panic`: prints/handles panic and does not return.
- `console_log`: writes text (`flags=0` stdout, `flags=1` stderr).
- `alloc`: allocate memory block.
- `resize`: resize/reallocate memory block.
- `free`: release memory block.

Concrete default platform implementation used by `slc compile`/`slc run`:
- `lib/platform_libc.c`
