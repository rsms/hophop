# SL Transpiler Project

## 1. Overview

### 1.1 What we’re building

A **C99 transpiler** for a custom minimal programming language (**SL**) designed for:

* highly portable **single-header libraries** and small programs,
* output that can compile **freestanding** (e.g. `-ffreestanding`, wasm),
* a syntax that is **regular and LLM-friendly**.

The transpiler itself is split into:

1. **Core library**: a single-header C99 library, freestanding-friendly, **no libc** assumptions (no malloc, no stdio, no printf).
2. **CLI tool**: a normal host C99 program (may use libc) that loads files, runs the library, and writes output.

### 1.2 Non-goals

* Full C interop with arbitrary headers (we can add `extern` later).
* Heavy optimization.
* Advanced generics/templates (duplicate-by-hand is fine for v0).
* Sophisticated module systems beyond Go-like packages.

### 1.3 Feature proposals

* SLP-1 variable-size structs: `docs/SLP-1-variable-size-structs.md`

---

## 2. Output Contract (Generated C99)

### 2.1 Single-header library layout

For an output header `foo.h` from package `foo`:

* Always emits:

  * an **implicit prelude** (types, bool, assert hooks, string helpers),
  * exported declarations (types and function prototypes).

* Optionally emits definitions under an implementation macro:

  * `#ifdef FOO_IMPL` … definitions … `#endif`

* No libc required in generated code.

* Internal/package-private definitions are either:

  * emitted under `#ifdef FOO_IMPL` as `static` functions, and/or
  * emitted as `static inline` if you choose that policy (v0: keep under `*_IMPL`).

### 2.2 Namespacing / mangling

SL has Go-like packages. C has none, so we mangle symbols.

**Rule:**

* For any declaration in package `pkg`, its C name is:

  * `pkg__Name` (double underscore separator)
* Package-private items can also be mangled (recommended for debug consistency), but must be `static` to avoid link conflicts.

Example:

* SL `heap.PQueue` → C `heap__PQueue`
* SL `heap.Push` → C `heap__Push`

### 2.3 Field access lowering

SL uses only `x.y`. Codegen chooses `.` vs `->` based on the **typed** receiver:

* If `x: T` (struct/union value): `x.y`
* If `x: *T` (pointer): `x->y`

### 2.4 Strings

SL `str` is a pointer to a blob: **u32 length + bytes + nul** (Redis-like):

* `"hi"` → `u32(2)` + `'h'` + `'i'` + `'\0'`.

The `str` value points at the `u32` length field.

Prelude provides:

* `len(s str) u32` (length in bytes, excluding nul)
* `cstr(s str) *u8` (pointer to nul-terminated bytes)

### 2.5 Assert

SL has `assert` as a keyword statement with optional formatted args. Lowering calls prelude hooks:

* `assert cond` →

  * if `!(cond)`: `SL_ASSERT_FAIL(__FILE__, __LINE__, "assertion failed")`
* `assert cond, "fmt", a, b` →

  * if `!(cond)`: `SL_ASSERTF_FAIL(__FILE__, __LINE__, "fmt", a, b)`

Defaults in prelude trap; projects may override macros.

---

## 3. SL Language Specification (v0)

### 3.1 Lexical rules

#### Comments

* Only `//` line comments.

#### Semicolons (Go-style insertion)

Semicolons are optional. The lexer inserts a semicolon at end-of-line if the last token on the line is:

* identifier
* literal (number/string/bool)
* keyword: `break`, `continue`, `return`
* closing delimiter: `)`, `]`, `}`

You may write explicit `;` anywhere a statement terminator is valid.

#### Identifiers

`[A-Za-z_][A-Za-z0-9_]*`

#### Literals

* Integers: decimal `123`, hex `0xFF` (binary optional)
* Floats: `1.0`, `1e-3`
* Bool: `true`, `false`
* Strings: `"..."` with escapes: `\\ \" \n \t \r \0 \xNN`

---

### 3.2 Packages and imports (Go-like namespaces)

There is no `package` keyword.

A package is inferred from filesystem layout:

* A package may span multiple `.sl` files in a directory.
* The package name is inferred from the directory name.
* For single-file package mode, the package name is inferred from the containing directory
  (fallback: filename without `.sl`).

#### Imports

v0 import model:

```sl
import "ds/heap"
import h "ds/heap"        // alias
```

Default import alias is inferred from the last path component.

Examples:

* `import "foo/bar"` binds alias `bar`.
* `import bar "foo/bar-v2"` is required if the last path component is not a valid identifier.

* Imported names are referenced via `pkg.Name` (or alias): `heap.Push`, `h.PQueue`.
* No dot-imports, no unnamed imports in v0.
* Cyclic imports are an error.

#### Export surface (`pub`)

Each package declares exports by marking top-level declarations with `pub`.

Allowed:

* `pub struct/union/enum` declarations
* `pub fn` declarations/definitions
* `pub const` declarations **only if compile-time** (optional, but recommended)

Everything not marked `pub` is **package-private** by default.

Example:

```sl
pub struct T { x i32 }
pub fn A(t T) i32;

fn A(t T) i32 { return t.x }
fn b(x i32) i32 { return x + 1 } // private
```

**Visibility rules:**

* Outside package: only names marked `pub` are accessible as `foo.T`, `foo.A`.
* Inside package: both exported and private are accessible.
* Exception for programs: `fn main` is treated as an entry point and does not need `pub`.

**API closure rule (recommended for simplicity):**

* Exported function signatures and exported type fields must reference only:

  * built-in types, or
  * other exported types, or
  * pointers/arrays thereof.
* Referring to private types from public API is a compile error.

---

### 3.3 Types

#### Built-in types

`u8 u16 u32 u64 i8 i16 i32 i64 usize isize f32 f64 bool void str`

#### Derived

* Pointer: `*T`
* Fixed array: `[N]T` where `N` is a compile-time integer constant.
* Named aggregates:

  * `struct Name { field Type ... }`
  * `union Name { field Type ... }`
  * `enum Name UnderlyingInt { ... }` (underlying integer type required)

#### Field declarations

In `struct/union` bodies:

```sl
fieldName Type
```

---

### 3.4 Declarations

#### Functions

Prototype:

```sl
fn Name(a i32, b i32) i32
```

Definition:

```sl
fn Name(a i32, b i32) i32 { ... }
```

No overloading. A function name is unique within its package.

#### Declaration order independence

Declaration order does not affect meaning inside a package:

* Types may reference other types declared later in the same package.
* Functions may call functions declared/defined later in the same package.
* Mutually recursive functions are allowed.

This implies the frontend/typechecker must collect declarations before validating bodies/usages.

#### Variables

Local variables (no inference):

```sl
var x i32 = 0
var buf [64]u8
```

(Top-level vars can be supported later; v0 can restrict to const + fn + types.)

#### Constants

v0 recommendation:

* `const` must have explicit type and compile-time initializer:

```sl
const N i32 = 16
```

(You can later add untyped consts.)

---

### 3.5 Statements and control flow

#### Blocks

`{ stmt* }`

#### If

```sl
if cond { ... } else { ... }
```

No parentheses required.

#### Loop (only `for`)

Three forms:

```sl
for { ... }                     // infinite
for cond { ... }                // condition
for init; cond; post { ... }    // C-ish
```

`init` may declare variables:

```sl
for var i i32 = 0; i < n; i += 1 { ... }
```

#### Switch (two modes)

Expression switch:

```sl
switch x {
    case 1 { ... }
    case 2, 3 { ... }
    default { ... }
}
```

Condition switch:

```sl
switch {
    case x < 0 { ... }
    case x < 10 { ... }
    default { ... }
}
```

**Case bodies are blocks** semantically, even if implemented as such syntactically:

* `defer` inside case works.
* Variables declared in a case are scoped to that case body.

**Lowering rule:**

* If expression switch and all case labels are compile-time constants → emit C `switch`.
* Otherwise → emit `if / else if / else` chain.

#### Break/continue/return

* `break`
* `continue`
* `return`
* `return expr`

`break` exits the innermost `for` or `switch`. `continue` applies only to `for`.

No `goto`.

---

### 3.6 Defer (block-scoped, Zig-like)

Defer executes when exiting the **current lexical block**.

Forms:

```sl
defer { stmt* }
defer stmt
```

`defer stmt` is equivalent to `defer { stmt }`.

Semantics:

* Deferred actions run **LIFO** order on block exit.
* Block exit occurs on:

  * falling off end of the block,
  * `break` leaving the block,
  * `continue` leaving the block,
  * `return` leaving the block (and therefore all enclosing blocks).

---

### 3.7 Expressions

#### Operators

C-like precedence, minus ++/-- (not present).
Assignments: `=`, and `+= -= *= /= %= &= |= ^= <<= >>=`

#### Field access

`x.y` (auto `.`/`->` based on type)

#### Indexing

`a[i]` where `a` is `[N]T` or `*T` (v0 allows pointer indexing).

#### Calls

`f(x, y)`

#### Cast

Postfix cast with high precedence:

```sl
x as i64
x * (y as i64)
```

#### Compound literals (named fields only)

```sl
T { field = expr, other = expr }
```

All fields must be named; missing fields default to zero (C designated init behavior).

---

### 3.8 Strict typing

#### Exact-type rule

Binary operations require operands of **exact same type**:

* arithmetic, bit ops, comparisons all require exact match.
* logical ops only on `bool`.

Assignments require exact type match.

#### Untyped numeric literals

* Integer literals are `untyped_int`
* Float literals are `untyped_float`

They become concrete only in a context with an expected type:

* variable initialization with explicit type,
* assignment to typed variable,
* argument to typed parameter,
* case label under typed switch expression.

Coercion must be representable; otherwise compile-time error.

---

## 4. Implementation Plan

## 4.1 Repository structure

Suggested layout:

```
sl/
  include/
    sl_transpiler.h        // single-header core library (freestanding-friendly)
  src/
    slc.c                  // CLI tool
  tests/
    cases/
      heap.sl
      strings.sl
      defer.sl
      switch_const.sl
      switch_cond.sl
    expected/
      heap.h
      ...
```

## 4.2 Core library design (single-header, no libc)

### 4.2.1 No-libc constraints

* No `malloc/free`, no `stdio`, no `string.h`.
* Provide your own minimal:

  * arena allocator on caller-provided buffer,
  * byte copy/set helpers (or require them via hooks).

### 4.2.2 Public API (C)

The library should expose “parse → typecheck → emit”:

```c
typedef struct sl_arena sl_arena;
typedef struct sl_diag  sl_diag;

typedef struct {
    const char* ptr;
    unsigned    len;
} sl_strview;

typedef struct {
    void* ctx;
    void (*write)(void* ctx, const char* data, unsigned len);
} sl_writer;

typedef struct {
    // output controls
    const char* impl_macro;      // e.g. "FOO_IMPL"
    const char* header_guard;    // e.g. "FOO_H"
    int emit_prelude;            // 1
    int emit_internal;           // 1 under impl macro
} sl_emit_opts;

// Parses a single file (package loader is done in CLI for v0)
int sl_parse_file(sl_arena*, sl_strview src, /*out*/ void** ast, sl_diag*);

// Resolves package-level symbols, types, imports (if you push into lib later)
int sl_typecheck(sl_arena*, void* ast, sl_diag*);

// Emits C99 for a package or compilation unit
int sl_emit_c99(sl_arena*, void* ast, sl_writer*, const sl_emit_opts*, sl_diag*);
```

For v0, you can keep “package loading / filesystem imports” in the CLI and pass the library a fully concatenated unit (or a list of ASTs). If you expect wasm embedding later, consider moving import resolution into the library, but it’s not required.

### 4.2.3 Diagnostics

Design a minimal diagnostic system with spans:

* file id
* byte offset start/end
* message (static string id + formatted detail optional)

Since core library is freestanding, avoid dynamic formatting. Have the CLI format user-facing output.

## 4.3 Frontend pipeline

### 4.3.1 Lexer

Responsibilities:

* tokenize input,
* implement semicolon insertion,
* produce token stream with source offsets.

Token kinds needed:

* identifiers
* keywords: `import pub struct union enum fn var const if else for switch case default break continue return defer assert`
* operators, delimiters, literals.

### 4.3.2 Parser

Implement a straightforward recursive descent parser:

* import decls
* pub-prefixed top-level declarations
* top-level type decls and function decls/defs
* statements and expressions

Expression parsing: Pratt parser is ideal and small.

AST nodes should store:

* kind enum
* child pointers/indices
* source span
* type pointer filled during typecheck

### 4.3.3 Name resolution (packages + selectors)

Within a package:

* two namespaces:

  * types (struct/union/enum)
  * values (fn/const/var)
    No overloading, so symbol table is simple.

For imports:

* `import alias "path"` binds alias -> imported package id.
* `alias.Name` resolves by looking up exported symbol `Name` in imported package export table.

Within a package, resolution is declaration-order independent:

* collect type and value declarations first,
* then resolve references and typecheck bodies.

### 4.3.4 Export surface building (`pub`)

Scan all files in the package:

* collect all declarations marked `pub` as exported symbols.
* validate uniqueness.
* validate that each exported `fn` has a matching definition somewhere (exact signature).
* validate public API closure rule (no private types leak).

### 4.3.5 Type system & typecheck

Represent types as canonical nodes:

* builtin
* pointer
* array
* named type (struct/union/enum)
* function type (for signatures only)

Typecheck tasks:

* resolve identifiers to symbols
* annotate AST with types
* enforce strict typing for operations and assignments
* implement untyped literal coercion at coercion sites
* validate switch typing rules
* validate `x.y` selector rule and record whether receiver is pointer (for codegen)

### 4.3.6 Control-flow-aware lowering requirements

You don’t need a full CFG, but `defer` requires understanding “what scopes are exited by break/continue/return”.

Approach:

* During codegen, maintain a **stack of active blocks**.
* Each block tracks a list of deferred statements.
* When emitting a jump that exits scopes, emit defers for all blocks being exited, in reverse order per block, and from innermost to outermost.

To do this, codegen needs to know:

* For `break`: which block depth is the loop body boundary.
* For `continue`: similarly.
* For `return`: exit all active blocks.

Implementation detail:

* When entering a loop body block, push a marker capturing:

  * `break_target_depth`
  * `continue_target_depth`
* When encountering `break`, compute how many blocks will be exited and emit their defers.

This is easiest if your AST preserves explicit blocks for:

* function body
* if/else bodies
* for body
* each case body

## 4.4 Code generation (C99 emitter)

### 4.4.1 Emitter strategy

A direct “pretty printer” is fine. Keep formatting deterministic.

Codegen phases:

1. Emit header guard / prelude.
2. Emit exported declarations (from `pub` declarations).
3. Emit `#ifdef PKG_IMPL` section:

   * emit private type defs (if needed)
   * emit private function defs (static)
   * emit exported function defs
4. Close guards.

### 4.4.2 Prelude emission

The transpiler injects a fixed prelude into every output header. Keep it configurable but stable.

Prelude should define:

* scalar types (`u8`, `i32`, etc.). Prefer compiler builtins where available, otherwise fallback.
* `bool` + `true/false`
* `sl_str` representation + helpers `len/cstr`
* `SL_TRAP()` / assert hooks

Example prelude skeleton (conceptual):

```c
typedef unsigned char u8;
typedef signed int    i32;
typedef unsigned int  u32;
typedef /*...*/       usize;
typedef _Bool         bool;

typedef const u8* str;

typedef struct { u32 len; u8 bytes[1]; } sl_strhdr;
static inline u32 len(str s) { return ((const sl_strhdr*)(const void*)s)->len; }
static inline const u8* cstr(str s) { return ((const sl_strhdr*)(const void*)s)->bytes; }

#ifndef SL_TRAP
    #if defined(__clang__) || defined(__GNUC__)
        #define SL_TRAP() __builtin_trap()
    #else
        #define SL_TRAP() do { *(volatile int*)0 = 0; } while(0)
    #endif
#endif

#ifndef SL_ASSERT_FAIL
    #define SL_ASSERT_FAIL(file,line,msg) SL_TRAP()
#endif
#ifndef SL_ASSERTF_FAIL
    #define SL_ASSERTF_FAIL(file,line,fmt,...) SL_ASSERT_FAIL(file,line,fmt)
#endif
```

### 4.4.3 String literal emission

Maintain a string-literal pool and emit one static object per distinct literal:

```c
typedef struct { u32 len; u8 bytes[N+1]; } sl_lit_42_t;
static const sl_lit_42_t sl_lit_42 = { N, { ... , 0 } };
```

Then SL `"foo"` expression emits as `(str)(const u8*)&sl_lit_42`.

### 4.4.4 Switch emission strategy

* Expression switch:

  * If all case labels are compile-time constants: emit C `switch`.
  * Else: emit `if/else if`.
* Condition switch: always emit `if/else if`.

Ensure each `case` body is treated as a block for `defer`.

### 4.4.5 Cast emission

`x as T` → `((T)(x))` with parentheses.

### 4.4.6 Field selector emission

Use type annotation of the receiver:

* value → `.`
* pointer → `->`

### 4.4.7 Defer lowering in codegen

Emit defers inline before jumps.

Example approach:

* When generating code for a block:

  * collect `defer` statements into the block’s defer list
  * do not emit them immediately
* When exiting the block normally (end of block):

  * emit its defers in reverse order
* When encountering `break/continue/return`:

  * emit defers for all blocks that will be exited, innermost-first, each block LIFO
  * then emit the jump/return

This avoids creating many labels and keeps the generated C readable.

## 4.5 CLI tool plan (hosted, uses libc)

Responsibilities:

* Accept entry package path (directory) or single file.
* Load all `.sl` files belonging to the package.
* Resolve imports by locating packages by path.
* Build an import graph (DAG), detect cycles.
* For “package build” mode:

  * transpile just that package and emit one header.
* Optional “bundle build” mode:

  * emit one header containing root package and all dependencies (later).

CLI flags (suggested):

* `-pkg <dir>` package directory
* `-o <file>` output header
* `-impl-macro <NAME>` override default `<PKG>_IMPL`
* `-guard <NAME>` override header guard
* `-bundle` include imported packages too (future)

The CLI can allocate large buffers and pass them to the core library arena.

---

## 5. Testing Plan

### 5.1 Golden tests

For each `tests/cases/*.sl`, produce `tests/expected/*.h`.
Test:

* transpiler output matches expected (exact text).

Include cases:

* semicolon insertion
* strict typing errors
* selector `. vs ->`
* defer in nested blocks and in switch cases
* expression switch lowering to switch vs if/else
* string literal layout + `len/cstr`
* pub export enforcement + private symbol errors
* import resolution and `pkg.Name`

### 5.2 Compile tests

For each expected header:

* compile as hosted (`clang -std=c99 -Wall -Wextra`)
* compile as freestanding (`clang -std=c99 -ffreestanding -fno-builtin`)
  No linking required; compile-only is fine.

---

## 6. Example: Priority Queue Package (SL)

### `ds/heap/heap.sl`

```sl
pub struct PQueue {
    data *i32
    len  i32
    cap  i32
}

pub fn Init(q *PQueue, backing *i32, cap i32) void;
pub fn Push(q *PQueue, x i32) bool;
pub fn Pop(q *PQueue, out *i32) bool;
pub fn Peek(q *PQueue, out *i32) bool;

fn Init(q *PQueue, backing *i32, cap i32) void {
    q.data = backing
    q.len  = 0
    q.cap  = cap
}

fn Peek(q *PQueue, out *i32) bool {
    if q.len == 0 { return false }
    *out = q.data[0]
    return true
}

fn Push(q *PQueue, x i32) bool {
    if q.len >= q.cap { return false }

    var i i32 = q.len
    q.len += 1
    q.data[i] = x

    for i > 0 {
        var p i32 = (i - 1) / 2
        if q.data[p] >= q.data[i] { break }
        var t i32 = q.data[p]
        q.data[p] = q.data[i]
        q.data[i] = t
        i = p
    }

    return true
}

fn Pop(q *PQueue, out *i32) bool {
    if q.len == 0 { return false }
    *out = q.data[0]
    q.len -= 1
    if q.len == 0 { return true }

    q.data[0] = q.data[q.len]

    var i i32 = 0
    for {
        var l i32 = i*2 + 1
        var r i32 = l + 1
        if l >= q.len { break }

        var j i32 = l
        if r < q.len && q.data[r] > q.data[l] {
            j = r
        }

        if q.data[i] >= q.data[j] { break }

        var t i32 = q.data[i]
        q.data[i] = q.data[j]
        q.data[j] = t
        i = j
    }

    return true
}
```

### `app/main.sl`

```sl
import heap "ds/heap"

fn main() i32 {
    var backing [64]i32
    var q heap.PQueue
    heap.Init(&q, &backing[0], 64 as i32)

    assert heap.Push(&q, 10)
    assert heap.Push(&q, 30)
    assert heap.Push(&q, 5)

    var x i32
    assert heap.Pop(&q, &x)
    assert x == 30
    return 0
}
```

---

## 7. Build Phases (recommended order)

### Phase 0: Skeleton

* arena allocator, token stream, diagnostics
* minimal CLI that reads a single file and tokenizes

### Phase 1: Parser + pretty-printer

* parse types, functions, blocks, statements, expressions
* emit a debug AST (for tests)

### Phase 2: Typecheck core

* symbol tables (types + values)
* strict typing, literals coercion
* selector `x.y` resolution and pointer/value annotation
* cast `as`

### Phase 3: Defer + switch semantics

* block stack with defers
* switch parsing, case bodies as blocks
* switch lowering decision

### Phase 4: Packages + imports + `pub`

* package loader in CLI: load all `.sl` in dir
* parse multiple units
* merge `pub` exports
* resolve imports and `pkg.Name`
* enforce API closure rule
* codegen as single-header package

### Phase 5: Strings + assert + prelude stabilization

* literal pool + correct layout
* `len/cstr` lowering
* `assert` lowering

### Phase 6: Testing + polish

* golden outputs
* compile tests hosted + freestanding
* error messages with spans

---

## 8. Notes for the coding agent (implementation tips)

* Keep AST nodes compact and arena-allocated.
* Use integer IDs for symbols and types (intern everything).
* Treat `pub` declarations as the single source of export truth.
* In v0, keep import resolution simple: map import path → directory → parse package.
* Codegen should be type-driven (no guessing `. vs ->`, no guessing switch lowering).
