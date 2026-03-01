# SLP-22 consteval diagnostics

## Summary

SLP-22 allows const-evaluated SL code to emit compiler diagnostics intentionally.

This enables library-authored compile-time checks with precise messages, instead of relying only on
generic typechecker failures.

Surface:

```sl
import "compiler"
import "reflect"

compiler.error("message")
compiler.error_at(reflect.Span{}, "message")
compiler.warn("message")
compiler.warn_at(reflect.Span{}, "message")
```

## Motivation

As reflection and consteval become central to generic-style APIs, libraries need to enforce
compile-time constraints themselves.

Example: a pure-SL `fmt` implementation should be able to report:

- invalid format pattern
- placeholder/argument count mismatch
- argument type mismatch for a placeholder

with diagnostics anchored to useful source spans.

Without this feature, users see lower-quality errors or backend-specific behavior.

## Goals

- Allow consteval code to emit errors/warnings with custom messages.
- Keep diagnostics backend-agnostic and deterministic.
- Support span targeting so diagnostics point to relevant source code.

## Non-goals

- Arbitrary runtime diagnostics from non-const code.
- Full macro/preprocessor system.
- User-defined diagnostic categories/codes in v1.

## Proposed API

### `reflect` package additions

```sl
pub struct Span {
    file  &str = current_source_file
    start struct { line, column u32 }
    end   struct { line, column u32 }
}
pub fn span_of(operand &anything) Span
```

`Span` can be constructed to manufacture custom spans, i.e. `Span{ start: { line: 4, column: 12 } }`, just like with any other struct type. However it is a special type in the sense that the compiler knows about it as "the source span type" and initializes it with values of the current location of use rather than zeroes. I.e. `var here Span` is the span of the `here` symbol while `var here_ish = Span{ end: { line: 0, column: 0 } }` is given a zero `end` value but a valid `file` and `start` value. `Span` can be useful default-initialized in other types, to capture the location of "instantiation" of a value. E.g. `struct Thing { span reflect.Span }; var thing Thing` here `thing.span` describes the location of the `thing` variable.

`span_of(operand)` produces a `Span` value describing the provided named symbol. The operand can be anything material with a name: a const or variable, a function, a type and so on. It cannot be a built-in (name cannot start with `__sl_`) nor can it be a keyword (e.g. `span_of(var)` in invalid.) `span_of` is not a "real" function; the compiler detects uses of `reflect.span_of` and replaces it with an appropriate `Span`.

### `compiler` package (new)

```sl
pub fn error(message &str)
pub fn error_at(span &reflect.Span, message &str)
pub fn warn(message &str)
pub fn warn_at(span &reflect.Span, message &str)
```

- `message` must be const-evaluable `&str`

## Semantics

### 1. Where calls are valid

`compiler.*` diagnostics are valid only while code is being evaluated by consteval.

If a call is reachable only at runtime, it is a compile-time error to use these APIs there.

### 2. Error behavior

`compiler.error*`:

- emits one compiler error diagnostic
- aborts current consteval evaluation path
- causes containing expression/declaration to fail typecheck

### 3. Warning behavior

`compiler.warn*`:

- emits one compiler warning diagnostic
- does not abort evaluation

### 4. Span selection

- `error`/`warn` report at current consteval call site
- `error_at`/`warn_at` report at provided `Span`
- `reflect.span_of(x)` returns the parsed source span of operand `x`

### 5. Determinism

For identical source and build configuration, emitted diagnostics must be deterministic in content
and ordering.

## Interaction with consteval execution

This SLP requires consteval runtime/evaluator hooks for diagnostics.

Conceptually:

- consteval builtin calls are intercepted by evaluator
- evaluator forwards diagnostic payload to compiler diagnostic engine
- errors short-circuit evaluation in the same way as other consteval fatal failures

## Diagnostics (compiler-internal)

Suggested internal diagnostics for misuse:

- `consteval_diag_non_const_context`: consteval diagnostics API used outside consteval context
- `consteval_diag_message_not_const_string`: diagnostic message is not const-evaluable string
- `consteval_diag_invalid_span`: provided span is invalid

## Examples

### 1. Simple compile-time assertion helper

```sl
import "compiler"

fn require(cond bool, msg &str) {
    if !cond {
        compiler.error(msg)
    }
}
```

### 2. Error anchored to argument span

```sl
import "compiler"

fn require_int(arg_span &reflect.Span, is_int bool) {
    if !is_int {
        compiler.error_at(arg_span, "expected integer argument")
    }
}
```

### 3. Warning

```sl
import "compiler"

fn warn_deprecated(used bool) {
    if used {
        compiler.warn("this API is deprecated")
    }
}
```

## Test plan

1. Positive:
- error emitted from const initializer path
- warning emitted from const initializer path
- span-targeted diagnostic points to expected token range

2. Negative:
- use in runtime-only function body rejected
- non-const message rejected

3. Determinism:
- stable message text and ordering across repeated runs

## Open questions

1. Should duplicate warnings from repeated consteval instantiations be deduplicated by default?
    - Answer: yes, deduplicate per unique reflect.Span
