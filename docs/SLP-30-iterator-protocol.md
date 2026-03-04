# SLP-30 iterator protocol for `for ... in` user types (draft)

## Summary

SLP-30 extends `for ... in` (SLP-29) with a protocol for user-defined iterable types.

This SLP adds no new syntax. It defines a lowering contract based on two functions:

- `__iterator(source) -> Iter`
- `advance(it *Iter, <out>) bool` where `out` is a value pointer and optionally a key pointer.

The protocol is designed to support:

- immutable-reference binding (`for value in expr`)
- mutable-pointer binding (`for *value in expr`)
- optional key (`for key, value in expr`)


## Motivation

SLP-29 covers sequence-like types (`len` + `[]`) but leaves user-defined/infinite iteration out of scope.

This SLP adds a minimal protocol that:

- is pure library-level convention (no parser changes)
- maps to straightforward `for` lowering
- supports finite and infinite iterables
- preserves backend optimization opportunities through explicit state and out-params

## Protocol surface

For source expression `src`, an iterable protocol implementation consists of:

```sl
fn __iterator(src <SourceType>) <IterType>
```

and one or more `advance` functions:

```sl
fn advance(it *<IterType>, valueOut *&T) bool   // immutable reference binding
fn advance(it *<IterType>, valueOut **T) bool   // mutable pointer binding
fn advance(it *<IterType>, keyOut *&K, valueOut *&T) bool
fn advance(it *<IterType>, keyOut *&K, valueOut **T) bool
fn advance(it *<IterType>, keyOut **K, valueOut *&T) bool
fn advance(it *<IterType>, keyOut **K, valueOut **T) bool
```

Implementor may chose to implement these functions in anyway that makes `advance` callable as described above. For example, `anytype` could be leveraged for compile-time monomorphization:

```sl
fn advance(it *<IterType>, result ...anytype) bool
```

Rules:

- `__iterator` initializes iterator state.
- `advance` writes the next item into `out`, updates iterator state, and returns:
  - `true` when an item was produced
  - `false` when iteration is exhausted
- Iterator types may implement only a subset of binding modes (`*&T`, `**T`).

## Semantic rules

### 1. Iterable resolution order

Given `for ... in EXPR`:

1. If type of `EXPR` is a built-in sequence type (for example `&str` or `*[i64]`), use SLP-29 sequence lowering.
2. If `__iterator(typeof(EXPR))` is available, attempt protocol lowering. Stop on error.
2. [PLANNED FOR THE FUTURE, NOT PART OF SLP-30] If `len(typeof(EXPR))` and `__get_element(typeof(EXPR), uint)` are available, use SLP-29 sequence lowering.
3. If none applies, emit `for_in_invalid_source`.

### 2. Single evaluation of source expression

`EXPR` is evaluated exactly once before loop entry and stored in an implementation temporary used by
`__iterator`.

### 3. Iterator state lifetime

Iterator state is a local variable scoped to the lowered loop.

### 4. Binding-mode to `advance` signature mapping

For each loop form:

- `for value in EXPR { ... }`
  - requires `advance(*Iter, *T) bool`
  - local binding is `var value T`
- `for &value in EXPR { ... }`
  - requires `advance(*Iter, *&T) bool`
  - local binding is `var value &T`
- `for *value in EXPR { ... }`
  - requires `advance(*Iter, **T) bool`
  - local binding is `var value *T`

Partial support is valid: an iterator may provide only one or two of these overload families.

If the loop requests a mode that the iterator does not implement, emit
`for_in_advance_no_matching_overload` at compile time.
Diagnostic must include:

- requested loop binding mode (`T`, `&T`, or `*T`)
- iterator type selected by `__iterator`
- supported `advance` out-parameter forms for that iterator type

### 5. Value discard (`_`)

For `for _ in EXPR { ... }`:

- if `advance(*Iter) bool` exists, it is used
- otherwise, compiler may synthesize a temporary and call any available two-argument `advance`
  overload

If no valid `advance` form exists, emit `for_in_advance_no_matching_overload`.

### 6. Control flow

`break`, `continue`, and `defer` behavior is unchanged because lowering targets ordinary `for`
loops.

## Canonical example

```sl
struct List {
    head ?*ListEntry
}
struct ListEntry {
    value int
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

fn advance(it *MutListIterator, result **ListEntry) bool {
    if it.next != null {
        *result = it.next!
        it.next = it.next.next
        return true
    }
    return false
}
fn advance(it *ListIterator, result *&ListEntry) bool {
    if it.next != null {
        *result = it.next!
        it.next = it.next.next
        return true
    }
    return false
}
fn advance(it *ListIterator, result *ListEntry) bool {
    if it.next != null {
        *result = *(it.next!)
        it.next = it.next.next
        return true
    }
    return false
}
```

## Canonical lowering

```sl
fn modify(list *List) {
    {
        var entry *ListEntry
        for var __sl_tmp1 MutListIterator = list.__iterator(); __sl_tmp1.advance(&entry); {
            entry.value *= 100
        }
    }
}
fn read_only_ref(list &List) {
    {
        var entry &ListEntry
        for var __sl_tmp1 ListIterator = list.__iterator(); __sl_tmp1.advance(&entry); {
            assert entry.value >= 100
        }
    }
}
fn read_only(list &List) {
    {
        var entry ListEntry
        for var __sl_tmp1 ListIterator = list.__iterator(); __sl_tmp1.advance(&entry); {
            assert entry.value >= 100
        }
    }
}
```

## Optional helper pattern

To reduce duplicated iterator logic, implementations may use `anytype` helper functions and keep
public `advance` overloads as thin wrappers. This is non-normative and not required by the
protocol.

## Diagnostics

Recommended new diagnostics:

- `for_in_invalid_source`: source is neither sequence-iterable (SLP-29) nor protocol-iterable
- `for_in_iterator_ambiguous`: `__iterator(EXPR)` overload resolution is ambiguous
- `for_in_advance_no_matching_overload`: requested binding mode is unsupported; include requested
  mode and available `advance` out-parameter forms for the selected iterator type
- `for_in_advance_non_bool`: selected `advance` does not return `bool`

## Compatibility

This is additive.

- Existing SLP-29 sequence lowering remains unchanged and preferred.
- Existing code without iterator protocol methods is unaffected.

## Implementation notes

- parser: no changes
- typechecker:
  - keep existing SLP-29 fast path first
  - otherwise resolve `__iterator(EXPR)` and selected `advance` overload by binding mode
  - synthesize typed loop-local binding from `advance` out-parameter
- lowering/codegen:
  - emit straightforward `for init; cond; post` form where condition is `advance(...)`
  - ensure `EXPR` is evaluated once

## Test plan

Add tests for:

1. Positive:
   - protocol iteration with `for value`, `for &value`, `for *value`
   - finite and infinite iterator shapes (with explicit `break` for infinite)
   - source expression evaluated once
2. Negative:
   - missing `__iterator`
   - missing required `advance` overload for selected binding mode
   - non-`bool` `advance` return
   - ambiguous `__iterator`/`advance` resolution
3. Interaction:
   - `continue`/`break`/`defer` behavior matches equivalent lowered `for`
   - sequence types still use SLP-29 lowering path

## Non-goals

SLP-30 does not add:

- new loop syntax
- key/value protocol for `for key, value in ...` (deferred)
- ordering guarantees beyond what iterator implementation defines
