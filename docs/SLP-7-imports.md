# SLP-7 import declarations (completed)

## Summary

SLP-7 revises `import` to support:

- `as` aliases after the path.
- Side-effect-only imports via `as _`.
- Named symbol imports from a package.
- Optional package alias + named imports in one declaration.
- Comma or semicolon separators in named-import lists.

Proposed examples:

```sl
import "foo/bar"              // package imported as symbol 'bar'
import "foo/bar" as bar2      // package imported as symbol 'bar2'
import "foo/bar" as _         // side-effect import only

import "foo/bar" { bottle }   // import symbol 'bottle' as local 'bottle'
import "foo/bar" as pkg { bottle }
import "cat" { x, y as y2 }

import "fruit" {
    apple, banana
    citrus
    peach as not_a_plum
}
```

`import "pkg" { * }` is explicitly out of scope and rejected.
This draft includes concrete decisions intended to make implementation planning direct.

---

## Motivation

Current import syntax (`import alias "path"` or `import "path"`) is limited:

- Alias position is inconsistent with the rest of the language (`as` is already used in expressions).
- Side-effect-only imports require dummy aliases and are unclear.
- Accessing one or two symbols requires qualifying through package alias every time.
- Multi-line import lists are not available.

SLP-7 improves readability and scales better for packages with mixed usage patterns.

---

## Syntax

Replace import grammar with:

```ebnf
ImportDecl       = "import" StringLit [ImportAlias] [ImportSymbols] ";" ;
ImportAlias      = "as" (Ident | "_") ;
ImportSymbols    = "{" [ImportSymbol { ImportSep ImportSymbol } [ImportSep]] "}" ;
ImportSymbol     = Ident [ "as" Ident ] ;
ImportSep        = "," | ";" ;
```

Notes:

- Semicolon insertion already treats newline as statement terminator after identifiers, so newline-separated symbols in `{ ... }` work via implicit `;`.
- `*` is not valid inside `ImportSymbols`.

---

## Semantic rules

### 1. Package binding behavior

Given `import "path" ...`:

- No `as`, no `{...}`:
  - Bind package under inferred alias from last path segment.
- `as name`, no `{...}`:
  - Bind package as `name`.
- `as _`, no `{...}`:
  - Import package for side effects; do not bind a package name.
- `{...}` without `as`:
  - Import listed symbols only; do not bind package name.
- `{...}` with `as name`:
  - Bind package as `name` and import listed symbols.
- `{...}` with `as _`:
  - Invalid (redundant/ambiguous form).

### 2. Named symbol imports

For each symbol item:

- `x` binds exported symbol `x` from target package as local name `x`.
- `x as y` binds exported symbol `x` from target package as local name `y`.

Constraints:

- Source symbol must be exported by target package.
- Local binding names must be valid identifiers and not reserved.
- `_` is not allowed as a symbol alias (`import "p" { x as _ }` is invalid).
- Duplicate local bindings within one import declaration are rejected.
- `as _` is only allowed for pure side-effect imports (`import "p" as _`).

### 3. Name conflict rules

An import declaration is rejected when it introduces any local binding that conflicts with:

- another imported binding in the same file scope,
- an existing top-level declaration in the package scope,
- the package alias from the same declaration.

### 4. Path validation

After string-literal decoding, import path must satisfy:

- not empty,
- not absolute (must not start with `/`),
- not exactly `.` or `..`,
- no leading or trailing whitespace,
- characters limited to `[A-Za-z0-9_./-]`,
- no empty path segments (forbid `//`),
- path may contain `.` and `..` segments and is lexically normalized before resolution.

Normalization rules:

- Remove `.` segments.
- Resolve `..` by popping one preceding segment.
- If normalization escapes above loader root, reject import.

If default package alias is needed (no `as`, no `{...}`), alias inference uses the last path
segment of the normalized path and requires a valid identifier (`[A-Za-z_][A-Za-z0-9_]*`).
Inference is exact and does not transform/sanitize/split the segment.
Examples of disallowed inference behavior:

- no `b-c -> c`
- no `pkg.v2 -> pkg`

Example:

```sl
import "./bad-name."
```

is rejected with: `cannot infer package identifier`.

Explicit alias is valid in that case:

```sl
import "./bad-name." as bad
```

Likewise:

```sl
import "a/b-c"
```

is rejected and must use explicit alias:

```sl
import "a/b-c" as b_c
```

### 5. Special imports

- `import "slang/feature/<name>"` remains a compiler directive import.
- `import "platform"` remains a built-in package import.
- Feature imports must be path-only (`import "slang/feature/<name>"`) and cannot use `as`
  or `{...}`.
- `platform` follows normal SLP-7 import rules (alias and named imports are allowed).

---

## Invalid examples

```sl
import ""            // invalid import path (empty path)
import "/"           // invalid import path (absolute path)
import "."           // invalid import path (cannot import itself)
import " "           // invalid import path (leading whitespace)
import "a:"          // invalid import path (invalid character)
import "a/b-c"       // cannot infer package identifier; requires explicit alias
import "pkg" { * }   // wildcard import is not supported
```

---

## Diagnostics

Recommended new diagnostics:

- `import_invalid_path_empty`
- `import_invalid_path_absolute`
- `import_invalid_path_dot`
- `import_invalid_path_whitespace`
- `import_invalid_path_char`
- `import_invalid_path_segment`
- `import_invalid_path_escape_root`
- `import_alias_inference_failed`
- `import_symbol_not_exported`
- `import_symbol_duplicate_local`
- `import_binding_conflict`
- `import_wildcard_not_supported`
- `import_symbol_alias_invalid`
- `import_side_effect_alias_with_symbols`
- `import_feature_import_extras`

Existing diagnostics may be reused where appropriate (for example reserved identifier rules).

---

## Compatibility and migration

This proposal changes source syntax:

- old: `import alias "path"`
- new: `import "path" as alias`

Absolute imports (`import "/x/y"`) become invalid.

Suggested migration strategy:

1. Parser accepts both forms for one transition period.
2. Old form emits deprecation warning.
3. Old form is removed in a later release.

---

## Implementation notes

Likely compiler changes:

- `parse.c`:
  - Extend import parser to parse `StringLit`, optional `as`, optional `{...}` list.
  - Add AST representation for import symbol entries.
- `slc.c` package loader:
  - Track import mode (package alias, side-effect-only, symbol list).
  - Validate and normalize path under new rules.
  - Resolve named symbols against target package exported declarations.
- name resolution/checking:
  - Add top-level bindings for named imports.
  - Preserve existing `alias.Name` resolution for package aliases.
- code generation:
  - Update import-based symbol rewrite pass to account for direct symbol bindings.

---

## Non-goals

SLP-7 does not add:

- wildcard imports (`import "pkg" { * }`),
- module re-export syntax via import declarations,
- per-symbol visibility controls (`pub import ...`),
- package-version syntax in import paths.

---

## Proposed decisions (ready to implement)

1. `import "pkg" { x }` imports only listed symbols and does not bind inferred package alias.
2. `as _` is only legal without symbol list; `import "pkg" as _ { x }` is rejected.
3. Built-in `platform` uses the same alias rules as normal packages in SLP-7.
4. Named imports from `platform` are allowed.
5. Feature imports are path-only directives and reject `as` and `{...}`.
6. If alias inference fails, explicit alias is accepted.
   Inference is exact final-segment-only; no transformations are applied.
7. Path segments may include `.` in names (for example `math.v2`); `.` and `..` path segments are
   normalized with root-escape rejection.
8. All binding collisions introduced by imports are hard errors (no shadowing).
9. Multiple imports of same normalized package path are allowed; loading is deduplicated by path,
   and symbol bindings are additive subject to duplicate-binding errors.
10. Diagnostic precedence is:
    1) parse/syntax errors,
    2) path decode/validation/normalization errors,
    3) feature-import form errors,
    4) alias inference errors,
    5) symbol-export resolution errors,
    6) binding conflict errors.

## Remaining ambiguities

None identified at draft level.
