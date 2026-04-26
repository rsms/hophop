# HEP-26 format functions (completed)

## Summary

HEP-26 originally added a formatter buffer API. The current API is part of the implicit `builtin`
package and is available without an explicit import:

```hop
format("values = {}, {}", 123, 456)
format_str(runtime_fmt, 123, 456)
```

Signature:

```hop
fn format(const format &str, args ...anytype) *str
fn format_str(format &str, args ...anytype) *str
```

Behavior:

- `format` allocates a `*str` for const-evaluable format strings
- `format_str(format &str, ...)` allocates a `*str` for non-const format strings and validates at runtime

Scope for v1 placeholders:

- `{}`: default integer, floating-point, or string rendering
- `{i}`: integer types
- `{f}`: floating-point types
- `{s}`: `str`-compatible types

More complex formatting (for example structs) is deferred to a future version.

This proposal intentionally leaves existing `builtin.fmt` unchanged.

## Motivation

Current `fmt` has compiler-specific behavior and broad reflective formatting support.

This proposal created a narrower API that can be implemented in pure HopHop using HEP-24 (`const`
parameters) and HEP-25 (`anytype` variadics). The completed API lives in `builtin`, so callers use
it directly.

## API surface

Location:

- `lib/builtin/format.hop`

Import style:

```hop
format("values = {}, {}", 123, 456)
format_str(runtime_fmt, 123, 456)
```

Function:

```hop
fn format(const format &str, args ...anytype) *str
fn format_str(format &str, args ...anytype) *str
```

Notes:

- existing `builtin.fmt` remains available and unchanged
- callers use the implicit builtin import; the former explicit `str` formatter import is not
  supported
- `format(const format, ...)` is the allocating helper with compile-time validation
- `format_str(format &str, ...)` is the allocating helper for non-const format strings

## Formatting grammar (v1)

Supported tokens:

- literal text
- escape `{{` for literal `{`
- escape `}}` for literal `}`
- placeholders `{i}`, `{f}`, `{s}`

All other brace forms are invalid format strings. (More forms will be added in future changes, but not part of this HEP.)

## Semantic rules

### 1. Format validation timing

`format(const format, ...)` uses a `const` parameter (HEP-24), so the format string must be
const-evaluable at call site. The allocating `format_str(format &str, ...)` accepts non-const
format strings and validates at runtime.

Implementation validates placeholders/count/type at compile time in pure HopHop using `const { ... }`
logic (enabled by HEP-28 primitives). Invalid format shape/count/type fails during typecheck at
the call site.

### 2. Placeholder type compatibility

- `{i}` accepts integer types
- `{f}` accepts floating-point types (`f32`, `f64`, and aliases with float base)
- `{s}` accepts values assignable to `&str`

### 3. Anytype usage

`args ...anytype` is heterogeneous.

Current implementation is pure HopHop and uses explicit index-specialized dispatch.
Current v1 implementation limit is 16 variadic arguments.

## Diagnostics

Recommended new diagnostics (names illustrative):

- `format_invalid`: malformed format token/brace usage
- `format_arg_count_mismatch`: placeholder count does not match argument count
- `format_arg_type_mismatch`: placeholder incompatible with argument type
- `format_const_required`: `format` parameter argument is not const-evaluable

## Compatibility and migration

This is additive relative to `builtin.fmt`.

- no behavior changes to existing `builtin.fmt`
- users can migrate selectively by calling the builtin `format` or `format_str`

## Implementation notes

Reference implementation status:

- API surface is provided by `lib/builtin/format.hop`.
- Implementation is pure HopHop for validation and formatting logic. The C backend keeps a narrow
  helper to resolve allocated `format` template instances.
- Public signatures are `fn format(const format &str, args ...anytype) *str` and
  `fn format_str(format &str, args ...anytype) *str`.
- `format` placeholder validation is call-site compile-time (`const { ... }`).
- Allocating `format_str(format &str, ...)` runtime validation supports non-const format strings.

## Test plan

Add tests for:

1. Positive:
   - `{i}`, `{f}`, `{s}` with matching argument types
   - mixed placeholders and escaped braces
2. Negative:
   - invalid format token (`{x}`, unmatched brace)
   - placeholder/argument count mismatch
   - type mismatch per placeholder kind
   - runtime validation for non-const format arguments
3. Compatibility:
   - existing `builtin.fmt` tests continue to pass unchanged

## Non-goals

HEP-26 v1 does not add:

- struct/union/enum reflective formatting
- width/precision/alignment flags
- locale-aware formatting
- replacement/removal of existing `builtin.fmt`

## Resolved decisions

1. `{s}` accepts values that can be implicitly converted to `&str`.
   - accepted: `&str`, `*str`
   - rejected unless explicitly cast: non-`str` families like `&[u8]`
