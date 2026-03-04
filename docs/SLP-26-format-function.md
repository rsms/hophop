# SLP-26 str.format function (completed)

## Summary

SLP-26 adds a new formatting API in `lib/str/format.sl` that users import explicitly:

```sl
import "str" { format }
```

Signature:

```sl
fn format(buf *[u8], const format &str, args ...anytype) uint
```

Behavior:

- writes formatted output bytes into `buf`
- returns the number of bytes that would have been written if `buf` were infinitely large
- matches libc `snprintf` return-count contract
- if `len(buf) > 0`, `format` writes at most `len(buf)-1` payload bytes and writes a trailing
  NUL byte at the final position

Scope for v1 placeholders:

- `{i}`: integer types
- `{f}`: floating-point types
- `{s}`: `str`-compatible types

More complex formatting (for example structs) is deferred to a future version.

This proposal intentionally leaves existing `core.fmt` unchanged.

## Motivation

Current `fmt` has compiler-specific behavior and broad reflective formatting support.

This proposal creates a narrower, explicit, import-based API that can be implemented in pure SL
using SLP-24 (`const` parameters) and SLP-25 (`anytype` variadics).

## API surface

Location:

- `lib/str/format.sl`

Import style:

```sl
import "str" { format }
```

Function:

```sl
fn format(buf *[u8], const format &str, args ...anytype) uint
```

Notes:

- existing `core.fmt` remains available and unchanged
- callers opt in by importing `str` symbol `format` explicitly
- no separate companion helper is introduced in this SLP

## Formatting grammar (v1)

Supported tokens:

- literal text
- escape `{{` for literal `{`
- escape `}}` for literal `}`
- placeholders `{i}`, `{f}`, `{s}`

All other brace forms are invalid format strings. (More forms will be added in future changes, but not part of this SLP.)

## Semantic rules

### 1. Format validation timing

`format` parameter is `const` (SLP-24), so the format string must be const-evaluable at call site.

Implementation validates placeholders/count/type at compile time in pure SL using `const { ... }`
logic (enabled by SLP-28 primitives). Invalid format shape/count/type fails during typecheck at
the call site.

### 2. Placeholder type compatibility

- `{i}` accepts integer types
- `{f}` accepts floating-point types (`f32`, `f64`, and aliases with float base)
- `{s}` accepts values assignable to `&str`

### 3. Output/write contract

Let `cap = len(buf)`.

- function attempts to format full output logically
- writes at most `cap` bytes to `buf`
- return value is total logical output byte length, including truncated bytes
- truncation is not an error
- if `cap == 0`, writes nothing and still returns total required length
- if `len(buf) > 0`, `format` always writes a trailing NUL byte after payload bytes (or at index 0
  when payload is empty/truncated)

### 4. Anytype usage

`args ...anytype` is heterogeneous.

Current implementation is pure SL and uses explicit index-specialized dispatch.
Current v1 implementation limit is 16 variadic arguments.

## Diagnostics

Recommended new diagnostics (names illustrative):

- `format_invalid`: malformed format token/brace usage
- `format_arg_count_mismatch`: placeholder count does not match argument count
- `format_arg_type_mismatch`: placeholder incompatible with argument type
- `format_const_required`: `format` parameter argument is not const-evaluable

## Compatibility and migration

This is additive.

- no behavior changes to existing `core.fmt`
- users can migrate selectively by importing `str` symbol `format`

## Implementation notes

Reference implementation status:

- API surface is provided by `lib/str/format.sl`.
- Implementation is pure SL (no format-specific typecheck hooks, no format-specific C backend
  lowering).
- Public signature is `fn format(buf *[u8], const format &str, args ...anytype) uint`.
- Placeholder validation is call-site compile-time (`const { ... }`), not runtime panic-based.
- Runtime logic handles only formatting/writing after validation.

## Test plan

Add tests for:

1. Positive:
   - `{i}`, `{f}`, `{s}` with matching argument types
   - mixed placeholders and escaped braces
   - truncation behavior: return count > written bytes
   - zero-capacity buffer behavior
   - trailing NUL behavior in `format` itself when `len(buf) > 0`
2. Negative:
   - invalid format token (`{x}`, unmatched brace)
   - placeholder/argument count mismatch
   - type mismatch per placeholder kind
   - non-const format argument
3. Compatibility:
   - existing `core.fmt` tests continue to pass unchanged

## Non-goals

SLP-26 v1 does not add:

- struct/union/enum reflective formatting
- width/precision/alignment flags
- locale-aware formatting
- replacement/removal of existing `core.fmt`

## Resolved decisions

1. `{s}` accepts values that can be implicitly converted to `&str`.
   - accepted: `&str`, `*str`
   - rejected unless explicitly cast: non-`str` families like `&[u8]`
2. Apply NUL-termination behavior directly in `format`.
   - keep return-count contract unchanged (count excludes trailing NUL)
   - behavior:
     - if `len(buf) == 0`, no write
     - if `len(buf) > 0`, reserve one byte for terminating `0`
