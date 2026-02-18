# SL Library Reference

This document describes the built-in library surface of SL:
- built-in non-primitive types
- built-in functions
- platform package API

Primitive scalar types and language syntax are documented in `docs/language.md`.

## Built-In Types

### `str`

`str` is a specialized string type. Conceptually, it is a specialized form of `[u8]` where
the data is UTF-8 text.

Properties:
- `len(s)` is byte length.
- `cstr(s)` exposes a pointer to UTF-8 bytes for C interop.
- Use `[u8]`/`mut[u8]` for arbitrary binary data.

### `MemAllocator`

`MemAllocator` is the allocator capability used by `new(...)`.

Use sites:
- `new(ma, T)`
- `new(ma, T, N)`

The allocator argument must be convertible to `mut&MemAllocator`.

## Built-In Functions

### `len(x) -> u32`

Accepted argument categories:
- `str`
- arrays and slices
- pointers/references to arrays or slices

### `cstr(s) -> *u8`

`s` must be convertible to `str`.

### `new(ma, T[, N])`

Parameters:
- `ma`: convertible to `mut&MemAllocator`
- `T`: type argument expression
- optional `N`: integer expression

Return type:
- `*T` when `N` is omitted
- `*[T N]` when `N` is a positive compile-time constant
- `*[T]` when `N` is runtime-valued (or constant `<= 0`)

Restrictions:
- `T` cannot be a variable-size-by-value type.
- Negative constant `N` is rejected.

### `panic(msg) -> void`

`msg` must be convertible to `str`.

### `sizeof(Type)` / `sizeof(expr) -> uint`

Both forms are supported.

Notes:
- `sizeof(Type)` rejects variable-size-by-value types.
- `sizeof(expr)` supports runtime-sized values where applicable.

## Platform Package API

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
