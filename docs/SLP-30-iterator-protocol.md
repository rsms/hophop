# SLP-30 iterator protocol for `for ... in` user types (draft)

## Summary

SLP-30 extends `for ... in` (SLP-29) with a protocol for user-defined iterable types.

This revision uses:

- `__iterator(source) -> Iter`
- `next_value(it *Iter) -> ?(&T|*T)`
- `next_key(it *Iter) -> ?(&K|*K)` (optional, keyed discard preference)
- `next_key_and_value(it *Iter) -> ?*(K, V)` (optional, keyed/value fallback)

There is no `advance(..., out) bool` in this revision.

## Motivation

SLP-29 covers sequence-like types (`len` + `[]`) but leaves user-defined/infinite iteration out of scope.

The protocol in this revision keeps lowering simple while preserving explicit ownership semantics:

- `for value in source` copies from `*next_value(...)`
- `for &value in source` binds to `next_value(...)` directly
- keyed loops on custom iterators are supported through `next_key` / `next_key_and_value`

## Protocol surface

For source expression `src`, an iterable protocol implementation consists of:

```sl
fn __iterator(src <SourceType>) <IterType>
fn next_value(it *<IterType>) ?*T
fn next_value(it *<IterType>) ?&T
fn next_key(it *<IterType>) ?*K
fn next_key(it *<IterType>) ?&K
fn next_key_and_value(it *<IterType>) ?*(K, V)
```

`<SourceType>` may be by-value (`T`), immutable-reference (`&T`), or pointer (`*T`), using normal overload matching.

Rules:

- `__iterator` initializes iterator state.
- `next_*` hooks update iterator state and return:
  - `null` when iteration is exhausted
  - non-null pointer/ref payload when a value is produced

## Semantic rules

### 1. Iterable resolution order

Given `for ... in EXPR`:

1. If `EXPR` is built-in sequence-iterable, use SLP-29 sequence lowering.
2. Otherwise resolve `__iterator(EXPR)` with standard conversion-cost overload matching.
3. If not found and source is assignable, retry with autoref source type.
4. For iterator path, resolve hooks by loop form:
   - `for value in EXPR` / `for &value in EXPR` / `for _ in EXPR`:
     - prefer `next_value(*Iter)`
     - fallback to `next_key_and_value(*Iter)` (pair element 1)
   - `for key, value in EXPR` / `for key, &value in EXPR`:
     - use `next_key_and_value(*Iter)` (pair elements 0 and 1)
   - `for key, _ in EXPR`:
     - prefer `next_key(*Iter)`
     - fallback to `next_key_and_value(*Iter)` (pair element 0)
5. If none applies, emit `for_in_invalid_source`.

### 2. Single evaluation of source expression

`EXPR` is evaluated exactly once before loop entry and stored in an implementation temporary.

### 3. Binding-mode mapping

- `for value in EXPR`:
  - prefers `next_value(it *Iter) ?(&T|*T)`
  - fallback: `next_key_and_value(it *Iter) ?*(K, V)` (bind from `V`)
  - loop local has type `T` from dereferencing payload
- `for &value in EXPR`:
  - prefers `next_value(it *Iter) ?(&T|*T)`
  - fallback: `next_key_and_value(it *Iter) ?*(K, V)` (bind from `V`)
  - loop local has payload type (`&T` or `*T`)
- `for _ in EXPR`:
  - same hook resolution as value-binding forms
  - payload is discarded
- `for key, value in EXPR` / `for key, &value in EXPR`:
  - requires `next_key_and_value(it *Iter) ?*(K, V)`
  - key local type is `K`
  - value local type uses value-binding mode conversion from `V`
- `for key, _ in EXPR`:
  - prefers `next_key(it *Iter) ?(&K|*K)`
  - fallback: `next_key_and_value(it *Iter) ?*(K, V)`
  - key local type is `K`

### 4. Control flow

`break`, `continue`, and `defer` behavior is unchanged because lowering targets ordinary `for` loops.

## Canonical example

```sl
struct List {
    head ?*ListEntry
}
struct ListEntry {
    value i32
    next  ?*ListEntry
}
struct MutListIterator {
    next ?*ListEntry
}
struct ListIterator {
    next ?&ListEntry
}

fn __iterator(list *List) MutListIterator { return { next: list.head } }
fn __iterator(list &List) ListIterator { return { next: list.head } }

fn next_value(it *MutListIterator) ?*ListEntry {
    var cur = it.next
    if cur != null {
        it.next = cur.next
    }
    return cur
}

fn next_value(it *ListIterator) ?&ListEntry {
    var cur = it.next
    if cur != null {
        it.next = cur.next
    }
    return cur
}
```

## Diagnostics

Recommended diagnostics:

- `for_in_invalid_source`: source is neither sequence-iterable nor protocol-iterable
- `for_in_iterator_ambiguous`: `__iterator(EXPR)` overload resolution is ambiguous
- `for_in_advance_no_matching_overload`: no compatible `next_*` overloads for the requested loop form
- `for_in_advance_non_bool`: selected `next_*` hook does not return optional pointer/ref payload

## Compatibility

This revision is intentionally breaking versus previous SLP-30 drafts:

- removed `*value` for `for ... in`
- replaced `advance(..., out) bool` with `next_value(it *Iter) ?(&T|*T)`

## Test plan

Add tests for:

1. Positive:
   - protocol iteration with `for value`, `for &value`, `for _`
   - keyed protocol iteration with `for key, value`, `for key, &value`, `for key, _`
   - fallback precedence (`next_value` over `next_key_and_value`, `next_key` over `next_key_and_value`)
   - source expression evaluated once
2. Negative:
   - missing `__iterator`
   - missing required hook for each loop form
   - non-compatible `next_*` return type
   - ambiguous `__iterator` / `next_*` resolution
3. Interaction:
   - `continue`/`break`/`defer` behavior matches equivalent lowered `for`
   - sequence types still use SLP-29 path
