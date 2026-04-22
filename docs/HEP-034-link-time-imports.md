# HEP-34 foreign linkage directives

## Summary

HEP-34 adds a small directive syntax for foreign imports and exports:

- `@directive`
- `@directive(...)`

Directives attach to the following top-level declaration.

HEP-34 defines three concrete directives:

- `@wasm_import(module, field)`
- `@c_import(symbol)`
- `@export(name)`

Example:

```hop
@wasm_import("my_namespace", "twice")
fn double(v i32) i32

@wasm_import("my_namespace", "half")
fn half(v i32) i32

@c_import("helper")
fn helper(v i32) i32

@c_import("errno")
const errno i32

@export("mylib_quarter")
pub fn quarter(x i32) i32 {
    return x / 4
}

fn main() {
    assert double(quarter(24)) == 12
    assert half(helper(4)) == 2
}
```

This replaces the earlier HEP-34 draft based on special `import "external/..."` syntax and
`pub "c"`.

## Motivation

HopHop needs a direct way to express:

- foreign functions and globals provided by C-ABI link inputs
- Wasm imports provided at module instantiation time
- HopHop functions exported for foreign callers

The earlier `import "external/..."` design solved the problem, but it made the grammar and import
semantics significantly more complex. Directives keep the feature local to each declaration and let
imports and exports share one mechanism.

This approach is smaller:

- parser support is minimal
- ordinary package-import semantics stay unchanged
- raw Wasm module strings fit naturally as directive arguments
- exports no longer need a special `pub` variant

## Goals

- Add a minimal syntax for foreign-linkage metadata on declarations.
- Import foreign functions with Wasm or C ABI metadata.
- Import read-only and mutable foreign globals.
- Export selected HopHop functions for foreign callers.
- Keep ordinary `import` and `pub` semantics intact.
- Fail explicitly when a selected backend/toolchain path cannot lower the requested ABI surface.

## Non-goals

HEP-34 does not add:

- arbitrary C header parsing
- a generic user-extensible attribute system
- declaration directives on local declarations or statements
- `context` on foreign-imported or foreign-exported functions
- foreign exports for `const` or `var`
- foreign type imports or exports
- support for external imports on `cli-eval`
- arbitrary per-declaration ABI strings beyond the concrete directives in this HEP

## Syntax

Add directive syntax before top-level declarations:

```ebnf
SourceFile      = { ImportDecl ";" } { DirectiveTopDecl ";" } .
DirectiveTopDecl = { Directive } TopDecl .
Directive       = "@" Ident [ "(" [ DirectiveArgList ] ")" ] .
DirectiveArgList = Literal { "," Literal } [ "," ] .
Literal         = StringLit | RuneLit | IntLit | FloatLit | "true" | "false" | "null" .
```

Notes:

- HEP-34 only defines semantics for directives attached to top-level `fn`, `const`, and `var`
  declarations.
- The parser accepts a general directive surface, but the checker only recognizes the concrete
  directives defined in this HEP.
- In HEP-34, all directive arguments used by the standardized foreign-linkage directives are string
  literals.

## Semantic rules

### 1. General directive behavior

Directives attach to the following top-level declaration.

Rules:

- unknown directives are errors
- duplicate or conflicting foreign-linkage directives on one declaration are errors
- directive order does not change meaning
- directives are checked after parsing; the parser does not assign built-in semantics

For HEP-34:

- `@wasm_import` and `@c_import` are valid only on top-level declaration-only `fn`, `const`, and
  `var`
- `@export` is valid only on top-level `pub fn` definitions

### 2. Ordinary package imports remain unchanged

HEP-7 path imports and named-symbol imports continue to work unchanged.

This proposal does not add special import-path semantics. Foreign linkage is expressed only through
directives on declarations.

### 3. `@wasm_import(module, field)`

`@wasm_import(module, field)` declares that the following top-level declaration is imported from a
Wasm module/namespace at instantiation time.

Valid targets:

- declaration-only `fn`
- declaration-only `const`
- declaration-only `var`

Arguments:

- `module`: string literal naming the Wasm module/namespace
- `field`: string literal naming the imported field

Rules:

- both arguments are required
- both arguments must be string literals
- there is no validation or normalization of `module` or `field`
- the decoded string bytes are used as the Wasm import names
- the local HopHop binding name is the declaration name, not the `field` string

Example:

```hop
@wasm_import("lol-this.is some ;crazy? namespace", "half")
fn half(v i32) i32
```

### 4. `@c_import(symbol)`

`@c_import(symbol)` declares that the following top-level declaration is imported from an external
C-ABI symbol.

Valid targets:

- declaration-only `fn`
- declaration-only `const`
- declaration-only `var`

Arguments:

- `symbol`: string literal naming the foreign symbol

Rules:

- exactly one argument is required
- the argument must be a string literal
- the local HopHop binding name is the declaration name, not the `symbol` string

Example:

```hop
@c_import("zlib_crc32")
fn crc32(v i32) i32
```

Here the local HopHop name is `crc32`, while the foreign symbol name is `zlib_crc32`.

### 5. `@export(name)`

`@export(name)` declares that the following HopHop function is exported for foreign callers.

Valid target:

- `pub fn` definition

Arguments:

- `name`: string literal naming the foreign export

Rules:

- exactly one argument is required
- the argument must be a string literal
- `@export` does not change the HopHop declaration name
- `@export` and `pub` are orthogonal in intent, but HEP-34 requires `pub fn` for the initial
  design

Example:

```hop
@export("mylib_quarter")
pub fn quarter(x i32) i32 {
    return x / 4
}
```

### 6. Imported declarations must be declarations only

Declarations carrying `@wasm_import` or `@c_import` must not have bodies or initializers.

Valid:

```hop
@c_import("helper")
fn helper(v i32) i32

@c_import("errno")
const errno i32

@wasm_import("env", "counter")
var counter i32
```

Invalid:

```hop
@c_import("helper")
fn helper(v i32) i32 { return v }

@c_import("errno")
const errno i32 = 1
```

### 7. `context` is not supported

Functions carrying `@wasm_import`, `@c_import`, or `@export` may not use `context`.

This avoids introducing a hidden-parameter foreign ABI rule in HEP-34.

### 8. Type restrictions

A declaration carrying `@wasm_import`, `@c_import`, or `@export` is valid only if the selected
backend/toolchain path can lower the declaration's full type through the requested ABI surface.

Rules:

- if the compiler can determine during checking that the declaration type is unsupported for the
  selected backend path, it should report that during checking
- otherwise the backend must report a direct code-generation error
- the compiler must not silently change ABI, storage, mutability, or representation to force a
  match

This applies to:

- full function signatures
- imported `const` and `var` value types
- exported function signatures

### 9. Mutability of imported globals

Mutability follows the HopHop declaration form:

- `@... const x T` imports a read-only foreign global
- `@... var x T` imports a mutable foreign global

This applies to both `@c_import` and `@wasm_import`.

### 10. Target restrictions

Foreign imports require a target path that supports external linkage.

In particular:

- building for `cli-eval` with any `@wasm_import` or `@c_import` is an error
- `@wasm_import` is valid only when the selected backend/toolchain path is producing Wasm output
- `hop genpkg:c` is allowed to emit C for `@wasm_import` even when no downstream target triple is
  known yet
- when `hop` uses the C backend and the final target is known not to be Wasm, `@wasm_import` is a
  build-time error

These errors should be reported explicitly as soon as the compiler has enough target information to
decide.

### 11. C backend lowering for `@wasm_import`

For `@wasm_import`, the initial C-backend support is defined in terms of Clang-compatible Wasm
import attributes.

The intended lowering shape is:

- imported function:
  - `extern <ret> <local_c_name>(...) __attribute__((import_module("<module_bytes>"), import_name("<field_bytes>")));`
- imported read-only global:
  - `extern const <ctype> <local_c_name> __attribute__((import_module("<module_bytes>"), import_name("<field_bytes>")));`
- imported mutable global:
  - `extern <ctype> <local_c_name> __attribute__((import_module("<module_bytes>"), import_name("<field_bytes>")));`

Rules:

- `<module_bytes>` is the decoded `module` string escaped as a C string literal
- `<field_bytes>` is the decoded `field` string escaped as a C string literal
- `<local_c_name>` follows the local HopHop binding name or the backend's usual internal-mangling rule
- the foreign Wasm names come from directive arguments, not from the HopHop declaration name
- if the selected C toolchain path is known not to support this form while targeting Wasm, that is
  a code-generation error

Other C compilers are out of scope for the initial implementation.

### 12. Backend intent for `@c_import` and `@export`

`@c_import` backend intent:

- C backend:
  - lower to foreign declarations with C ABI linkage using the directive's symbol string
- Wasm backend:
  - lower according to the backend's C-ABI interop strategy for Wasm-targeted output

`@export` backend intent:

- C backend:
  - emit an externally visible C ABI function using the directive's export name
  - final object-level symbol decoration follows the downstream ABI/toolchain conventions
- Wasm backend:
  - export the function from the module under the directive's export name

### 13. Name conflicts

Directive-marked declarations follow ordinary top-level binding conflict rules.

In addition:

- multiple foreign-linkage directives on one declaration are invalid
- multiple declarations that bind the same local top-level name are invalid, regardless of foreign
  symbol/export names

## Diagnostics

Recommended new diagnostics:

- `directive_unknown`
- `directive_target_invalid`
- `directive_duplicate`
- `directive_arg_count_invalid`
- `directive_arg_type_invalid`
- `import_foreign_definition_invalid`
- `import_foreign_initializer_invalid`
- `import_foreign_context_unsupported`
- `import_foreign_type_unsupported`
- `import_foreign_target_unsupported`
- `import_foreign_eval_unsupported`
- `export_context_unsupported`
- `export_type_unsupported`
- `export_target_invalid`
- `export_pub_required`

Existing top-level binding conflict diagnostics may be reused where appropriate.

## Compatibility and migration

This proposal is additive:

- existing ordinary `import` forms are unchanged
- existing `pub fn` semantics are unchanged
- foreign linkage moves to directives instead of special import/export syntax

Migration from the earlier HEP-34 draft is mechanical:

- `import "external/wasm/<module>" { fn local(...) ... as alias }`
  becomes `@wasm_import("<module>", "<field>") fn alias(...) ...`
- `import "external/c" { fn local(...) ... }`
  becomes `@c_import("<symbol>") fn local(...) ...`
- `pub "c" fn name(...) ...`
  becomes `@export("<name>") pub fn name(...) ...`

## Implementation notes

Likely compiler changes:

- parser:
  - parse `@name` and `@name(...)` before top-level declarations
  - attach parsed directives to the following declaration node
  - parse directive argument lists as literals only
- checking:
  - resolve known directive names
  - validate legal target declaration kinds
  - validate argument count and literal kinds
  - reject `context` on imported/exported functions
  - reject bodies/initializers on imported declarations
  - reject conflicting foreign-linkage directives
- code generation:
  - C backend lowers `@wasm_import` using Clang-compatible `import_module` / `import_name`
    attributes
  - C backend lowers `@c_import` as foreign C declarations
  - C backend lowers `@export` as externally visible C ABI functions
  - Wasm backend lowers `@wasm_import` as Wasm imports using directive string arguments
  - Wasm backend lowers `@export` as Wasm exports using the directive string argument

## Proposed decisions

1. Add directive syntax `@directive` and `@directive(...)`, attached to the following top-level
   declaration.
2. HEP-34 standardizes only `@wasm_import`, `@c_import`, and `@export`.
3. `@wasm_import(module, field)` imports a declaration from a Wasm module/namespace and field named
   by two string literals, with no validation or normalization of those strings.
4. `@c_import(symbol)` imports a declaration from a C-ABI foreign symbol named by one string
   literal.
5. `@export(name)` exports a `pub fn` definition for foreign callers under the provided string
   literal name.
6. `@wasm_import` and `@c_import` are valid only on declaration-only top-level `fn`, `const`, and
   `var`.
7. `@export` is valid only on `pub fn` definitions.
8. Functions carrying `@wasm_import`, `@c_import`, or `@export` may not use `context`.
9. Foreign-linkage directives are valid only when the selected backend/toolchain path can lower the
   declaration type through the requested ABI.
10. `cli-eval` rejects programs that use foreign import directives.
11. The initial C-backend implementation of `@wasm_import` is defined in terms of
    Clang-compatible Wasm import attributes.
