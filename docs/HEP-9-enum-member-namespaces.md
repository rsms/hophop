# HEP-9 enum member namespaces (completed)

## Summary

HEP-9 changes enum member lookup from package-global names to enum-scoped names.

Current style:

```hop
enum Mode i32 {
    Mode_A = 0
    Mode_B = 1
}
fn is_mode_a(m Mode) bool {
    return m == Mode_A
}
```

Proposed style:

```hop
enum Mode i32 {
    A = 0
    B = 1
}
fn is_mode_a(m Mode) bool {
    return m == Mode.A
}
```

Core change:
- Enum item names are local to the enum declaration.
- Enum values are referenced through the enum namespace (`EnumName.Item`).

---

## Motivation

Package-global enum item names force artificial prefixes (`Mode_A`) to avoid collisions. This has a
few problems:

- noisy declarations and call sites
- accidental naming conventions leaking into API design
- collisions between unrelated enums in the same package

Using enum namespaces makes enum usage explicit and consistent with other qualified forms (`pkg.Name`).

---

## Syntax

Declaration grammar is unchanged:

```ebnf
EnumDecl = "enum" Ident Type "{" { EnumItem [ "," | ";" ] } "}" [";"] ;
EnumItem = Ident ["=" Expr] ;
```

Expression form for enum values is selector syntax already in the language:

```hop
Mode.A
pkg.Mode.A
```

No new token or keyword is introduced.

---

## Semantic rules

### 1. Enum member scope

- Enum item names are scoped to their enum only.
- Enum items do not create package-global value bindings.
- Duplicate item names are rejected within the same enum.
- Same item names in different enums are allowed.

### 2. Value lookup

- `E.X` resolves as enum member lookup when `E` resolves to an enum type.
- The type of `E.X` is `E`.
- `pkg.E.X` resolves through package alias/imported enum type then member.

### 3. Unqualified item names

- Bare names like `A` do not resolve to enum items.
- `return m == A` is invalid unless `A` is another symbol in scope.
- Intended usage is always qualified: `Mode.A`.

### 4. Imports and exports

- Exporting an enum exports the enum type, not standalone member symbols.
- Named imports continue to target top-level exported symbols only.
- `import "p" { Mode }` allows `Mode.A`.
- `import "p" { A }` is invalid (no top-level exported symbol `A` from enum members).

### 5. Initializer behavior

- Enum initializer expression rules are unchanged except for member naming lookup changes.
- Existing numeric compatibility/coercion behavior for enums remains unchanged in this HEP.

---

## Diagnostics

- `enum_member_unknown`: unknown enum member
- `enum_member_unqualified`: enum member must be referenced as `Enum.Member`

Fallback behavior may still use existing diagnostics (`unknown symbol`, `type mismatch`) where
implementation simplicity is preferred.

---

## Compatibility and migration

This is a source-level breaking change for code that references enum values as global names.

Migration pattern:

1. In enum declarations, rename members from prefixed style to local style when desired:
   - `Mode_A` -> `A`
2. Update all references:
   - `Mode_A` -> `Mode.A`

Mechanical migration is straightforward and can be automated safely in most cases.

---

## Implementation notes

### Parser

- No grammar changes required for enum declarations.
- No new AST node kind required.

### Typechecker

- Track enum members as per-enum symbols, not package-global value symbols.
- Extend selector resolution so `TypeName.Member` can resolve enum members.
- Ensure unqualified enum member names are not injected into top-level symbol lookup.

### Package loader / symbol export model

- Export tables remain type-centric for enums.
- Do not expose enum members as top-level package exports.

### C backend

- C enum enumerators are global in C; backend should mangle enumerator names to avoid collisions
  across enums while preserving HopHop-level `Enum.Member` semantics.
- Lower `Enum.Member` expressions to mangled C enumerators.

---

## Test plan

Add tests for:

1. Positive:
   - `Mode.A` lookup in same package
   - `pkg.Mode.A` lookup through imports
   - two enums with overlapping member names (`A`, `B`) in one package
2. Negative:
   - unqualified enum member reference (`A`) fails
   - unknown member (`Mode.C`) fails
   - named import of enum member (`import "p" { A }`) fails
3. Codegen:
   - generated C for enums with overlapping member names compiles without symbol collisions

---

## Non-goals

HEP-9 does not add:

- exhaustive enum switch checking
- enum methods
- scoped enum privacy controls beyond existing `pub` rules
- changes to enum underlying-type requirements

---

## Open questions and ambiguities

1. In enum initializer expressions, should prior enum members be usable unqualified (`B = A + 1`)
   or must they be qualified (`B = Mode.A + 1`)?
2. Should we add a temporary compatibility mode that accepts old global-style enum member
   references with a deprecation warning?
3. Should `enum_member_unqualified` be a dedicated diagnostic, or should we keep only `unknown symbol`
   with a hint when a likely enum member match exists?
