# SLP-26 core/str.format function (draft)

## Summary

SLP-26 adds a new formatting API in `lib/core/str/format.sl` that users import explicitly:

```sl
import "core/str" { format }
```

Signature:

```sl
fn format(buf *[u8], const format &str, args ...anytype) uint
```

Behavior:

- writes formatted output bytes into `buf`
- returns the number of bytes that would have been written if `buf` were infinitely large
- matches libc `snprintf` return-count contract
- `format` itself does not append a trailing NUL byte; C-string termination is handled by a
  dedicated helper variant in this SLP

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

- `lib/core/str/format.sl`

Import style:

```sl
import "core/str" { format }
```

Function:

```sl
fn format(buf *[u8], const format &str, args ...anytype) uint
```

Notes:

- existing `core.fmt` remains available and unchanged
- callers opt in by importing `core/str` symbol `format` explicitly
- include a companion C-interop helper variant that applies NUL-termination when space remains in
  `buf`

## Formatting grammar (v1)

Supported tokens:

- literal text
- escape `{{` for literal `{`
- escape `}}` for literal `}`
- placeholders `{i}`, `{f}`, `{s}`

All other brace forms are invalid format strings. (More forms will be added in future changes, but not part of this SLP.)

## Semantic rules

### 1. Compile-time format validation

`format` parameter is `const` (SLP-24), so the format string must be const-evaluable at call site.

The format string is parsed and validated at compile time:

- invalid token/brace forms are compile-time errors
- placeholder count must match argument count
- placeholder type compatibility is checked against each bound `anytype` argument

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
- no implicit trailing NUL byte is appended by `format`

### 4. Anytype usage

`args ...anytype` is heterogeneous and processed via compile-time type dispatch for each
placeholder/argument pair.

## Diagnostics

Recommended new diagnostics (names illustrative):

- `format_invalid`: malformed format token/brace usage
- `format_arg_count_mismatch`: placeholder count does not match argument count
- `format_arg_type_mismatch`: placeholder incompatible with argument type
- `format_const_required`: `format` parameter argument is not const-evaluable

## Compatibility and migration

This is additive.

- no behavior changes to existing `core.fmt`
- users can migrate selectively by importing `core/str` symbol `format`

## Implementation notes

Library implementation target:

- pure SL implementation in `lib/core/str/format.sl`

Expected dependencies:

- SLP-24 `const` parameter enforcement
- SLP-25 `...anytype` pack handling
- compile-time type checks via reflection operations already present/proposed for `type` values

No compiler special-case for function name `format` should be required.

## Test plan

Add tests for:

1. Positive:
   - `{i}`, `{f}`, `{s}` with matching argument types
   - mixed placeholders and escaped braces
   - truncation behavior: return count > written bytes
   - zero-capacity buffer behavior
   - C-interop helper writes trailing NUL when `bytes_written < len(buf)`
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
2. Include C-interop NUL-termination helper behavior in this SLP.
   - keep `format` return-count contract unchanged
   - helper behavior is a final conditional NUL write:
     `if bytes_written < len(buf) { buf[bytes_written] = 0 }`
