# HopHop Formatter Maintenance Notes

This document describes the formatting contract that maintainers should preserve (or change deliberately).
It is not an implementation walkthrough.

## Scope and goals

`hop fmt` is intended to be:

- deterministic
- idempotent (`fmt(fmt(x)) == fmt(x)`)
- comment-preserving
- stable across files and machines

The formatter is intentionally opinionated and currently does not attempt line-wrapping based on width.

## Primary behavior references

When behavior is unclear, these test fixtures are the contract:

- `tests/fmt_canonical.hop` (broad coverage)
- `tests/fmt_import_groups.expected.hop` (import sorting/alignment details)
- `tests/fmt_messy.expected.hop` (normalization from irregular input)
- `tests/fmt_dir_top.expected.hop` (directory mode expectations)

`fmt_canonical` should be treated as the language designer's style decision file.

## Grouping model (critical)

Many alignment rules are applied per group, not globally.

A new group starts when either is true between adjacent items:

- there is an empty line
- there is a line that contains only comments

A practical implication: comment-only separators reset alignment even when surrounding items are otherwise similar.

## Baseline whitespace

- Indentation uses tabs.
- A single trailing newline is required at end-of-file.
- Excess blank lines at EOF are removed.

## Comment behavior

- Leading comments stay attached above their anchored node.
- Trailing comments stay on the same line when possible.
- For aligned runs, trailing comments align by column inside each contiguous comment run.
- A non-commented line in the middle of a run breaks trailing-comment alignment continuity.
- Multi-line block comments are placed at the outer indentation level, and their interior bytes are
  preserved literally.

## Top-level declaration spacing

- Top-level declarations are separated by one blank line.
- Top-level `var`/`const` declarations preserve source blank-line grouping:
  adjacent declarations with no blank source line format as one aligned run, while one or more
  source blank lines collapse to one formatted blank line.
- Top-level `var`/`const` runs may align mixed `const` and `var` keywords in the same no-blank run.
- Import declarations are special-cased as import groups (see below).

## Import formatting

Within each import group:

- Imports are sorted by path (and alias as tie-breaker).
- Imports with symbol lists use one-line braces with interior spaces: `{ A, B }`.
- Symbol-list brace columns align within contiguous symbol-list runs.
- A plain import (no `{ ... }`) breaks symbol-list alignment runs.
- Trailing comments align within contiguous comment runs.

Import groups are not merged across blank/comment-only separators.

## One-line brace spacing rule

For one-line braced forms, interior spacing is canonicalized to `{ ... }`.
This includes:

- import symbol lists
- context overlay expressions
- compound literals
- inline switch clause bodies

Empty braces remain `{}`.

## Generic syntax formatting

- Declaration type-parameter lists render tight with comma-space separators:
  `struct Pair[A, B]`, `fn id[T](x T) T`, `type Ptr[T] *T`.
- Instantiated generic named types render the same way in type positions:
  `Pair[i32, i32]`, `&[Vector[i64]]`.
- Expression-context type values preserve the explicit `type` prefix:
  `typeof(x) == type Pair[i32, i32]`.
- Formatter output must preserve the language rule that function calls do not have explicit generic
  type arguments; syntax like `f[T](x)` is only round-tripped as ordinary postfix syntax.

## Alignment rules by construct

### Struct/union fields

- Field names and types align in columns by group.
- Defaults (`= value`) align in a column by group.
- Trailing comments align within contiguous comment runs.
- Grouped field syntax (`x, y, z T`) is preserved as grouped names.
- Embedded fields are not forced into the same alignment behavior as named-field rows.

### Enum members

- Members with `= value` align the `=` by group.
- Members without explicit value remain bare names.
- Trailing comments align within contiguous comment runs.

### Local `var`/`const`

- Name/type/`=`/initializer columns align by group.
- Typed and untyped declarations are allowed in the same group; alignment still applies.
- Trailing comments align within contiguous comment runs.

### Assignment statements

- Alignment applies to assignment expression statements by group.
- Alignment groups are conservative: only contiguous assignments with the same LHS text align together.
- This avoids broad alignment across unrelated assignment targets.

## Switch clause formatting

Switch clause behavior is intentionally nuanced.

- Clause groups follow the same separator rules (blank/comment-only line starts new group).
- Clause head columns are aligned for contiguous inline-body runs.
- Case/default bodies must remain blocks semantically.
- If a clause body block contains exactly one statement and no attached comments, it may be rendered inline as `{ stmt }`.
- Multi-line bodies render as normal blocks.

Brace alignment for multi-line clauses:

- At the start of a group, a multi-line clause uses single-space form: `case ... {`.
- If a multi-line clause immediately follows an inline-clause run in the same group, it may keep the aligned brace column of that run.
- After a group break, alignment resets.

This is why `fmt_canonical` can legitimately contain both:

- `case 0, 1 {` (unaligned group start)
- `case 5          {` (aligned after inline run)

## Defer formatting

- `defer` with a single-statement block is collapsed:
  - `defer { x += 1 }` -> `defer x += 1`
- Multi-statement blocks remain blocks.
- Grouping/alignment follows ordinary statement grouping behavior where relevant.

## Return simplification

- Redundant parentheses around return values are removed:
  - `return (expr)` -> `return expr`
- Parenthesized tuple returns normalize to expression-list form:
  - `return (a, b, c)` -> `return a, b, c`

## Redundant literal casts

- Redundant numeric literal casts are removed when the surrounding syntax fixes the same target type.
- Current supported contexts include typed `var` initializers, typed returns, direct generic-parameter returns, assignment RHS expressions whose target type can be inferred, matching binary operands including numeric comparisons, direct call arguments whose resolved parameter type exactly matches the cast target, and local field/call chains whose concrete type can be recovered from same-file generic declarations.
- Variadic direct-call tails are also simplified when the variadic element type exactly matches the cast target.
- Generic call-argument casts are only removed when the parameter type can be determined independently of the cast being removed. If the cast is what makes generic inference work, the formatter preserves it.
- Unresolved or ambiguous call targets are left unchanged.

## Redundant var declaration types

- Redundant single-name `var` declaration types are removed when the initializer independently fixes the same type.
- Current supported initializer contexts include explicit compound literals and local identifiers whose concrete type can be recovered from same-file declarations.
- Initializers that rely on the declaration type, such as untyped compound literals, `null`, and literal casts, keep the explicit `var` type.

## Expression operator spacing

Most binary operators render with spaces, but there is an intentional compatibility quirk:

- `*`, `/`, `%` may remain tight (`a*b`) in cases where source had no interior whitespace.

Do not "simplify" this without updating canonical tests and explicitly deciding the new style.

## Parser assumptions required by formatter output

Formatter output can emit one-line block bodies like `{ stmt }` in switch clauses.
This relies on parser acceptance of omitted trailing semicolon before `}` for statements inside blocks.
If parser behavior changes here, formatter output validity may regress.

## Change process for maintainers

When changing formatting behavior:

1. Update `tests/fmt_canonical.hop` first to express intended style.
2. Update focused formatter fixtures for edge cases (`fmt_import_groups`, `fmt_messy`, etc.).
3. Run formatter suite and full test suite.
4. Document non-obvious rule changes in this file.

If a rule is easy to infer from code and not style-ambiguous, this document does not need to restate it.
