# HopHop Library Reference

This document describes the built-in library surface of HopHop:
- built-in non-primitive types
- commonly used type forms (arrays, slices, pointers, references)
- built-in functions
- platform package API

Primitive scalar types and full language grammar are documented in `docs/language.md`.

## Type Forms (Arrays and Slices)

```hop
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

```hop
struct str {
    ptr *u8
    len int
}
```

Properties:
- `len(s)` is byte length.
- `cstr(s)` exposes a read-only reference to UTF-8 bytes for C interop.
- String literals assigned to `str` are validated to decode as UTF-8.
- Use `&[u8]` / `*[u8]` for arbitrary binary data.

### `rune`

`rune` is a builtin alias type:

```hop
type rune u32
```

Rune literals (`'a'`, `'\n'`, `'本'`) produce `rune` values.

### `__hop_MemAllocator`

`__hop_MemAllocator` is the low-level allocator capability used by `new`.
The allocator callback uses signed size and alignment values:

```hop
struct MemAllocator {
    impl fn(*MemAllocator, rawptr, int, int, *int, u32) rawptr
}
```

MemAllocator implementations must zero newly allocated bytes:
- fresh allocations are fully zeroed
- resized allocations must zero bytes in `[oldSize, newSize)`

For normal code, use `MemAllocator` (a nominal alias of `__hop_MemAllocator`).

## Built-In Functions


### `len`

```hop
fn len(x Seq) int
```

`len` returns the logical length of a sequence.
`Seq` can be an array or slice type (including `str`) as a value, reference, or pointer.


### `cstr`

```hop
fn cstr(s str) *u8
```

`cstr` returns a read-only reference to null-terminated UTF-8 bytes suitable for C APIs.

### `copy`

```hop
fn copy(dst *[anytype], src &[anytype]) int
```

`copy` copies elements from `src` into `dst` and returns the number of copied elements.

- Copy count is `min(len(dst), len(src))`.
- Source and destination ranges may overlap (memmove semantics).
- String slices preserve their string shape:
  - `str[a:b]` has type `str`
  - `(*str)[a:b]` has type `*str`
  - `(&str)[a:b]` has type `&str`
- String interoperability:
  - `copy(*str, &str)` is allowed.
  - `copy(*[u8], &str)` is allowed.
  - `copy(*str, &[u8])` requires an explicit cast of `src` to `&str`.


### `new`

```hop
new T
new [T N]
new T context alloc
new [T N] context alloc
```

`new` allocates memory from a memory allocator.

- `alloc` must be convertible to `*MemAllocator` and must be non-null with a valid `impl`.
- Contextual forms (`new T`, `new [T N]`) use allocator capability `mem` from effective context.
- Effective context `mem` must be assignable to `*MemAllocator` and must be non-null with a valid `impl`.
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

```hop
fn panic(message str)
```

`panic` logs an error message and stops program execution.
Exact behavior is platform-specific; it lowers to the platform panic operation.


### `fmt`

```hop
fn fmt(format str, ...args) *str
```

`fmt` builds a newly allocated string using `context.mem`.

- Returns `*str`; caller owns the allocation and should `free(...)` it.
- Placeholders in v1:
  - `{i}` integer decimal
  - `{r}` reflective rendering
- Escapes:
  - `{{` emits `{`
  - `}}` emits `}`
- When `format` is const-evaluable, placeholder shape/count and argument compatibility are checked at typecheck time.
- For non-const format strings, validation is deferred to runtime parsing.


### `str.format`

Import:

```hop
import "str" { format }
```

Signature:

```hop
fn format(buf *[u8], const format &str, args ...anytype) int
```

Behavior:

- `str.format` is imported from package path `str`.
- Supported placeholders are `{i}`, `{f}`, `{s}` with escapes `{{` and `}}`.
- Placeholder count must match argument count.
- Placeholder compatibility: `{i}` integer, `{f}` float, `{s}` values assignable to `&str`.
- Validation runs in pure-HopHop `const { ... }` logic at call site and fails during typecheck.
- Current implementation supports up to 16 variadic arguments.
- Return value is the payload byte count that would have been written without truncation.
- If `len(buf) > 0`, implementation writes at most `len(buf)-1` payload bytes and writes a trailing NUL byte.


### `print`

```hop
fn print(message &str)
```

`print` writes a UTF-8 message to standard output and ends the message with a line break.
It is implemented by calling `context.logger.handler(&context.logger, message, LogLevel.Info, 0)`.


### `sizeof`

```hop
fn sizeof(type T) int
fn sizeof(expr E) int
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
- `platform.console_log(message, flags)`
- `platform.panic(message, flags)`

Operation semantics:
- `exit`: terminate process with status code.
- `console_log`: write a platform log message.
- `panic`: stop execution after reporting a fatal error message.

Allocation is provided by `MemAllocator` via `new`, with the platform setting
`context.mem` before `hop_main`.

Concrete default platform implementation used by `hop compile`:
- `lib/platform/cli-libc/platform.c`

`hop run` defaults to the evaluator host (`cli-eval`) unless `--platform` is provided.
Selected platform targets may provide the platform surface in HopHop using foreign-linkage directives.
The `wasm-min` target does this in `lib/platform/wasm-min/platform.hop` with `@wasm_import(...)`
declarations for `exit`, `console_log`, and `panic`.

For `--platform playbit`, the raw runtime ABI lives in `lib/platform/playbit/platform.hop`.
That package stays close to Playbit syscalls.

Most Playbit programs should import the thin convenience packages under `lib/playbit/` instead:

- `playbit/console`
- `playbit/event`
- `playbit/handle`
- `playbit/object`
- `playbit/signal`
- `playbit/thread`
- `playbit/time`
- `playbit/window`

### Draft delta (HEP-17, not implemented)

Planned additions to platform library surface:

- Base context in built-in `platform` package:

```hop
pub struct Context {
    mem     *MemAllocator
    console i32
    stdin   ?i32
    fs      ?__hop_FileSystem
}
```

- Target context packages under `platform/<target>` that compose the base:

```hop
import "platform"

pub struct Context {
    platform.Context
    // target-specific fields
}
```

This keeps a stable portable base (`platform.Context`) while allowing target extension.
`main` is planned to receive the selected target context type.
`__hop_FileSystem` is planned as a built-in type initially; future work may define
global host types in `prelude.hop` instead.

## Reflection Package (Draft)

`reflect` is proposed in HEP-18 as a compile-time reflection API.

Planned surface (draft):

- `reflect.Kind` enum for type categories (`Primitive`, `Alias`, `Struct`, etc.)
- Type reflection operations via `typeof` and type-value methods:
  - `.kind()`
  - `.base()` (for aliases)
  - `.fields()` (for aggregates)
- Type constructor operations on `type` values:
  - `ptr(T)`
  - `slice(T)`
  - `array(T, N)`

Sketch examples:

```hop
import "reflect"

type MyInt int
struct Foo { x, y int }

fn main() {
    var x i32
    assert typeof(x) == i32
    assert i32.kind() == reflect.Kind.Primitive
    assert MyInt.kind() == reflect.Kind.Alias
    assert MyInt.base() == int
    assert Foo.kind() == reflect.Kind.Struct
    assert Foo.fields().len() == 2
}
```

Exact signatures and typing rules are draft and defined in `docs/HEP-018-reflection.md`.

## Source Locations Package Surface (Provisional)

`builtin` provides source-location reflection:

- `builtin.SourceLocation`
- `builtin.source_location_of(x)`

`SourceLocation` has `file &str`, `start_line int`, `start_column int`, `end_line int`, and `end_column int` fields. `source_location_of(x)` returns the parsed source location of operand `x`; unqualified `source_location_of(x)` is accepted through the implicit builtin import.

## Compiler Diagnostics Package (Provisional)

`compiler` provides compiler diagnostics hooks:

- `compiler.error(message &str)`
- `compiler.error_at(location builtin.SourceLocation, message &str)`
- `compiler.warn(message &str)`
- `compiler.warn_at(location builtin.SourceLocation, message &str)`

In consteval, these calls emit diagnostics during const evaluation. In ordinary code, they emit
diagnostics only on compile-time-proven execution paths.

## C Interop Mapping

Common HopHop <-> C type correspondences:

- `&T` <-> `const T*`
- `*T` <-> `T*`
- `*[T N]` <-> `T*`
- `*[T]` <-> `struct { T* ptr; __hop_int len; }`
- `&[T N]` <-> `const T*`
- `&[T]` <-> `struct { const T* ptr; __hop_int len; }`
