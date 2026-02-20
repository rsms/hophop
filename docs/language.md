# SL Language Specification

This document specifies the language accepted by the current `slc` implementation in this repository.
It is the canonical grammar/spec document for the language.

Scope:
- Source language syntax and static semantics.
- Package/import/export behavior used by `checkpkg`, `genpkg`, `compile`, and `run`.
- Runtime-relevant language behavior that is fixed by current lowering.

Out of scope:
- Draft/proposed SLP features not implemented in code.
- Backend-internal naming details beyond language-visible behavior.

## 1. Lexical Grammar

### 1.1 Characters and whitespace
- Source is byte-oriented.
- Whitespace: `space`, `tab`, `\r`, `\n`, `\f`, `\v`.
- Comments: line comments only: `// ...` to end-of-line.
- No block comment syntax.

### 1.2 Identifiers
- Pattern: `[A-Za-z_][A-Za-z0-9_]*`.

### 1.3 Keywords
`import pub struct union enum fn var const type mut if else for switch case default break continue return defer assert sizeof true false as null context with`

### 1.4 Literals
- Integer: decimal (`123`) or hex (`0x7F`, `0X7F`).
- Float: decimal with optional fraction and/or exponent (`1.0`, `1.`, `1e3`, `1.2e-3`).
- String: `"..."` with escapes accepted by lexer.
- Bool: `true`, `false`.
- Null literal: `null`.

String escape decoding used by current tooling/codegen supports:
- `\\`, `\"`, `\n`, `\r`, `\t`, `\0`, `\xNN`.
- Unknown escapes are currently accepted and treated as the escaped character.

### 1.5 Semicolon insertion
A semicolon is inserted:
- At newline if previous token can end a statement.
- At EOF if previous token can end a statement.

Tokens that end a statement:
- `IDENT`, `INT`, `FLOAT`, `STRING`, `TRUE`, `FALSE`, `NULL`
- `context` (when used as an expression identifier)
- `break`, `continue`, `return`
- `)`, `]`, `}`
- `!` (covers postfix unwrap at line end)

## 2. File Structure

`SourceFile := ImportDecl* TopDecl* EOF`

Rules:
- Imports must appear before top-level declarations.
- Empty statements are allowed between declarations.

## 3. Concrete Syntax (EBNF)

```ebnf
ImportDecl      = "import" StringLit [ImportAlias] [ImportSymbols] ";" ;
ImportAlias     = "as" (Ident | "_") ;
ImportSymbols   = "{" [ImportSymbol { ImportSep ImportSymbol } [ImportSep]] "}" ;
ImportSymbol    = Ident [ "as" Ident ] ;
ImportSep       = "," | ";" ;

TopDecl         = ["pub"] (StructDecl | UnionDecl | EnumDecl | TypeAliasDecl | FnDeclOrDef | FnGroupDecl | ConstDecl) ;

StructDecl      = "struct" Ident "{" { StructFieldDecl [ "," | ";" ] } "}" [";"] ;
UnionDecl       = "union"  Ident "{" { FieldDecl [ "," | ";" ] } "}" [";"] ;
StructFieldDecl = FieldDecl | EmbeddedFieldDecl ;
FieldDecl       = Ident Type ;
EmbeddedFieldDecl = TypeName ;

EnumDecl        = "enum" Ident Type "{" { EnumItem [ "," | ";" ] } "}" [";"] ;
EnumItem        = Ident ["=" Expr] ;

FnDeclOrDef     = "fn" Ident "(" [ParamList] ")" [Type] [ContextClause] (";" | Block) ;
FnGroupDecl     = "fn" Ident "{" GroupMember {"," GroupMember} "}" ";" ;
GroupMember     = Ident | Ident "." Ident { "." Ident } ;
ParamList       = ParamGroup {"," ParamGroup} ;
ParamGroup      = Ident {"," Ident} Type ;
ContextClause   = "context" TypeName ;

ConstDecl       = "const" Ident ([Type] "=" Expr) ";" ;
TypeAliasDecl   = "type" Ident Type ";" ;

Type            = OptionalType
                | PtrType
                | RefType
                | MutRefType
                | SliceType
                | MutSliceType
                | ArrayType
                | VarArrayType
                | TypeName ;

OptionalType    = "?" Type ;
PtrType         = "*" Type ;
RefType         = "&" NonSliceType ;
MutRefType      = "mut" "&" NonSliceType ;
SliceType       = "[" Type "]" ;
MutSliceType    = "mut" "[" Type "]" ;
ArrayType       = "[" Type IntLit "]" ;
VarArrayType    = "[" Type "." Ident "]" ;

NonSliceType    = PtrType | RefType | MutRefType | ArrayType | VarArrayType | TypeName ;
TypeName        = Ident { "." Ident } ;

Block           = "{" { Stmt [";"] } "}" ;
Stmt            = Block
                | VarDeclStmt
                | ConstDecl
                | IfStmt
                | ForStmt
                | SwitchStmt
                | ReturnStmt
                | BreakStmt
                | ContinueStmt
                | DeferStmt
                | AssertStmt
                | ExprStmt ;

VarDeclStmt     = "var" Ident (Type ["=" Expr] | "=" Expr) ";" ;
IfStmt          = "if" Expr Block ["else" (IfStmt | Block)] ;

ForStmt         = "for" (
                    Block
                  | Expr Block
                  | [ForInit] ";" [Expr] ";" [Expr] Block
                  ) ;
ForInit         = ("var" Ident (Type ["=" Expr] | "=" Expr)) | Expr ;

SwitchStmt      = "switch" [Expr] "{" { CaseClause } [DefaultClause] "}" ;
CaseClause      = "case" Expr {"," Expr} Block ;
DefaultClause   = "default" Block ;

ReturnStmt      = "return" [Expr] ";" ;
BreakStmt       = "break" ";" ;
ContinueStmt    = "continue" ";" ;
DeferStmt       = "defer" (Block | Stmt) ;
AssertStmt      = "assert" Expr ["," Expr {"," Expr}] ";" ;
ExprStmt        = Expr ";" ;

Expr            = AssignExpr ;
AssignExpr      = LogicalOrExpr [AssignOp AssignExpr] ;
AssignOp        = "=" | "+=" | "-=" | "*=" | "/=" | "%="
                | "&=" | "|=" | "^=" | "<<=" | ">>=" ;

LogicalOrExpr   = LogicalAndExpr { "||" LogicalAndExpr } ;
LogicalAndExpr  = BitOrExpr { "&&" BitOrExpr } ;
BitOrExpr       = BitXorExpr { "|" BitXorExpr } ;
BitXorExpr      = BitAndExpr { "^" BitAndExpr } ;
BitAndExpr      = EqualityExpr { "&" EqualityExpr } ;
EqualityExpr    = RelExpr { ("==" | "!=") RelExpr } ;
RelExpr         = ShiftExpr { ("<" | ">" | "<=" | ">=") ShiftExpr } ;
ShiftExpr       = AddExpr { ("<<" | ">>") AddExpr } ;
AddExpr         = MulExpr { ("+" | "-") MulExpr } ;
MulExpr         = UnaryExpr { ("*" | "/" | "%") UnaryExpr } ;

UnaryExpr       = (("+" | "-" | "!" | "*" | "&") UnaryExpr) | PostfixExpr ;

PostfixExpr     = PrimaryExpr { PostfixSuffix } ;
PostfixSuffix   = CallWithContextSuffix | IndexSuffix | SelectorSuffix | CastSuffix | UnwrapSuffix ;
CallWithContextSuffix = CallSuffix [WithContextClause] ;
CallSuffix      = "(" [Expr {"," Expr}] ")" ;
WithContextClause = "with" ("context" | ContextOverlay) ;
ContextOverlay  = "{" [ContextBindList] "}" ;
ContextBindList = ContextBind {"," ContextBind} [","] ;
ContextBind     = Ident ["=" Expr] ;
IndexSuffix     = "[" Expr "]"
                | "[" [Expr] ":" [Expr] "]" ;
SelectorSuffix  = "." Ident ;
CastSuffix      = "as" Type ;
UnwrapSuffix    = "!" ;

PrimaryExpr     = Ident
                | IntLit
                | FloatLit
                | StringLit
                | BoolLit
                | "null"
                | "sizeof" "(" (Type | Expr) ")"
                | "(" Expr ")" ;
```

Notes:
- `&[T]` and `mut&[T]` are invalid type forms; use `[T]` and `mut[T]`.
- Compound literals (`Type{ field = expr }`) are not implemented in the current parser.
- `void` is a type name but is rejected as an explicit function return type. Omit return type for no return value.

## 4. Declarations and Scope

- Top-level declaration kinds: `struct`, `union`, `enum`, `fn`, `const`.
- `pub` is an attribute on a single top-level declaration.
- Top-level `var` is not supported.
- `const` declarations always require an initializer:
  - `const x = expr`
  - `const x T = expr`
- Local `var` declarations support both inferred and explicit forms:
  - `var x = expr`
  - `var x T`
  - `var x T = expr`
- Local scope is lexical by block.
- Name lookup for locals is nearest enclosing declaration.
- Functions and types are collected before body checking (declaration-order independence).

Function identity:
- Multiple declarations with identical signature are allowed.
- At most one definition body per function name/signature in a checked unit.
- For a given function name/signature, declarations/definition must use the same `context` clause.
- `context` is not an overload dimension.
- Explicit overload groups are supported:
  - `fn update{update_pet, update_ship}`
  - `fn pick{pick_a, foo.pick_b}`
  - Calls to `update(...)` resolve across the grouped members.

Type-function selector-call sugar:
- `x.f(a, b)` can resolve as `f(x, a, b)`.
- Field lookup has precedence:
  - if `x.f` resolves as a field, normal field-call rules apply.
  - selector-call sugar is only considered when no field named `f` exists.
- Selector sugar is call-form only in current implementation (`x.f` alone does not produce a callable value).

### 4.4 Contexts and capabilities (SLP-12)
- A function may declare `context T` where `T` is a named type.
- Such a function gets an implicit local binding named `context` (lowered as a hidden pointer parameter).
- Calls auto-forward the current context when no `with` clause is present.
- `with context` is explicit pass-through and equivalent to omitting `with`.
- `with { ... }` creates a call-local overlay:
  - binds must name fields in the caller's current context
  - duplicate bind names are rejected
  - shorthand `name` means `name = context.name`
- Context compatibility is structural-by-field and name-sensitive.
- Built-in capability use:
  - `print(msg)` requires `console` in effective context
  - `new(T[, N])` requires `mem` in effective context
- Entrypoint rule remains `fn main()` with no explicit `context` clause.
- Inside `main`, the implementation provides an implicit root context with fields:
  - `mem` (platform allocator)
  - `console` (platform console handle/flags)

### 4.1 Struct composition
- In `struct` declarations, the first field may be an embedded base using type-name-only syntax:
  - `struct B { A; y int }`
- Embedded base constraints:
  - allowed only in `struct` (not `union`)
  - must be the first field
  - at most one embedded field per struct
  - embedded type must be a named `struct` type
  - embedded cycles are rejected
- Field promotion:
  - selectors resolve direct fields first, then recursively through the embedded base chain
  - for example, if `C` embeds `B` and `B` embeds `A`, then `c.x` may resolve as `c.B.A.x`

### 4.2 Enum members
- Enum item names are scoped to their enum declaration.
- Enum items do not create package-global value bindings.
- Enum values are referenced through enum selectors:
  - `Mode.A`
  - `pkg.Mode.A`
- The type of `Mode.A` is `Mode`.
- Unqualified enum item references are not resolved as enum members:
  - `A` is invalid unless `A` is another symbol in scope.

### 4.3 Initialization and zero values
- `var x T` (without initializer) initializes `x` to the zero value of `T`.
- Zero-value semantics are recursive for aggregates:
  - scalars (`bool`, integers, floats): `0` / `false`
  - pointers/references/slice pointers/function pointers: null
  - arrays: each element zero-initialized
  - structs/unions: all storage bytes zeroed
- This applies to locals and to generated-storage declarations in codegen.
- `const` has no implicit default value and must be initialized explicitly.

## 5. Type System

### 5.1 Built-in types
- `void`, `bool`, `str`, `__sl_MemAllocator`
- `u8 u16 u32 u64 i8 i16 i32 i64 uint int f32 f64`

### 5.2 Constructed types
- Pointer: `*T`
- Reference: `&T` (readonly), `mut&T` (mutable)
- Slice: `[T]` (readonly), `mut[T]` (mutable)
- Fixed array: `[T N]` where `N` is an integer literal token
- Dependent array type form: `[T .lenField]` (restricted; see variable-size structs)
- Optional: `?T` (feature-gated; see section 8)
- Named types: `Name`, `pkg.Name`

### 5.3 Assignability
Exact type match is required except these implicit conversions:
- `untyped_int -> any integer type`
- `untyped_int -> any float type`
- `untyped_float -> any float type`
- `mut&T -> &T`
- `*T -> &T` and `*T -> mut&T`
- `&T` / `mut&T` -> `*T`
- `mut[S] -> [S]`
- `[S N] -> [S]` and `[S N] -> mut[S]`
- `*[S N] -> [S]` and `*[S N] -> mut[S]`
- `Derived -> Base` for embedded-base ancestry
- `&Derived -> &Base` and `mut&Derived -> mut&Base` for embedded-base ancestry
- `*Derived -> &Base` and `*Derived -> mut&Base` for embedded-base ancestry
- `T -> ?T`
- `null -> ?T`
- `?A -> ?B` if `A` is assignable to `B`
- `Alias -> Target` for nominal aliases declared as `type Alias Target`

Not implicit:
- `?T -> T` (requires unwrap or narrowing)
- readonly-to-mutable reference/slice conversions
- numeric widening between concrete numeric types (for example `i32 -> i64`)

### 5.4 Literals and default typing
- Integer literal expression type: `untyped_int`
- Float literal expression type: `untyped_float`
- `null` expression type: `null`

Type-inferred declarations (`var x = expr`, `const y = expr`) concretize untyped literals as:
- `untyped_int -> int`
- `untyped_float -> f64`

Inference rejects:
- `var/const x = null`
- `var/const x = <void-expression>`

## 6. Expressions and Operators

Precedence (high to low): postfix, unary, multiplicative, additive, shift, relational, equality, bit-and, bit-xor, bit-or, logical-and, logical-or, assignment.

Operator constraints:
- Unary `+ -`: numeric.
- Unary `!`: bool only.
- Unary `*`: pointer/reference dereference.
- Unary `&`: produces mutable reference (`mut&T`).

Assignment LHS must be assignable expression:
- identifier
- index expression (non-slice)
- field expression (except dependent VSS fields)
- dereference (`*expr`) of mutable location

Compound assignments (`+=`, etc.):
- Same assignability as `=`.
- LHS type must be numeric.

Logical operators:
- `&&`, `||` require bool operands.

Comparison operators:
- `==`, `!=`, `<`, `>`, `<=`, `>=` require operands coercible to one common type.
- Special case allowed: optional/null equality and inequality (`?T == null`, `null != ?T`, etc.).

Casts:
- `expr as Type` is explicit cast.

## 7. Statements and Control Flow

### 7.1 `if`
- Condition must be bool.
- `else if` and `else` supported.

### 7.2 `for`
Forms:
- `for { ... }`
- `for cond { ... }` where `cond` must be bool
- `for init; cond; post { ... }` where `cond` (if present) must be bool

### 7.3 `switch`
Forms:
- Expression switch: `switch expr { case ... }`
- Condition switch: `switch { case cond ... }`

Rules:
- At most one `default`.
- Case bodies are blocks.
- In expression switch, each case label must be assignable to subject type.
- In condition switch, each case label must be bool.
- No fallthrough semantics.

### 7.4 `break`, `continue`, `return`
- `break`: valid inside `for` or `switch`.
- `continue`: valid only inside `for`.
- `return`: expression required iff function return type is non-void.

### 7.5 `defer`
- `defer block` or `defer stmt`.
- Defers are block-scoped, LIFO within block.
- Defers execute on:
  - normal block exit,
  - `break`/`continue` leaving block,
  - `return` leaving block(s).

### 7.6 `assert`
- First argument must be bool.
- Optional additional args: `assert cond, fmt, args...`.
- If `fmt` exists it must be assignable to `str`.

## 8. Optional Types Feature (`slang/feature/optional`)

Enable per file:
- `import "slang/feature/optional"`

Without that import:
- `?` in type position is a parse/type error.

Allowed optional targets currently:
- pointer, reference, and slice families only.
- `?i32`-style value optionals are rejected.

Optional operations:
- `null` assignable only to `?T`.
- Postfix unwrap `x!` requires `x: ?T`, yields `T`, and traps on null at runtime.

Flow narrowing (locals only, including params since params are locals):
- Direct checks only:
  - `x == null`, `x != null`, `null == x`, `null != x`
- Branch narrowing:
  - In `if x == null { ... } else { ... }`, `x` is `null` in `then`, `T` in `else`.
  - In `if x != null { ... } else { ... }`, `x` is `T` in `then`, `null` in `else`.
- Continuation narrowing:
  - If `if x == null { <terminates> }` with no `else`, continuation narrows `x` to `T`.
  - If `if x != null { <terminates> }` with no `else`, continuation narrows `x` to `null`.

## 9. Built-in Functions and Forms

### 9.1 `len(x)`
- Accepted argument families:
  - `str`
  - arrays/slices
  - pointers/references to arrays/slices
- Return type: `u32`.
- Current generated runtime behavior for null pointer/ref-to-array/slice: yields `0`.
- Selector-call sugar is supported: `x.len()` is equivalent to `len(x)` when `x` has no field named `len`.

### 9.2 `cstr(s)`
- `s` must be convertible to `str`.
- Return type: `*u8`.

### 9.3 `new`
- Supported forms:
  - `new(ma, T[, N])`
  - `new(T[, N])` (contextual allocator form)
- In explicit form, `ma` must be convertible to `mut&__sl_MemAllocator`.
- In contextual form, effective context must provide field `mem` assignable to
  `mut&__sl_MemAllocator`.
- `T` must be a type argument expression (identifier naming builtin or named type).
- `N` (if present) must be integer-typed; constant negative values are rejected.
- Return type: `*T`.
- On successful allocation, newly allocated bytes are zero-initialized.
  - New allocation: full allocation is zeroed.
  - Resize/grow allocation: bytes in `[oldSize, newSize)` are zeroed.
- Selector-call sugar is supported: `ma.new(T[, N])` is equivalent to `new(ma, T[, N])`

Typical user code uses `std/mem.Allocator`, which is a nominal alias of `__sl_MemAllocator`.

### 9.4 `panic(msg)`
- `msg` must be convertible to `str`.
- Return type: `void`.

### 9.5 `sizeof`
- `sizeof(Type)` and `sizeof(expr)` are both supported.
- Result type: `uint`.
- `sizeof(Type)` rejects variable-size-by-value types.

### 9.6 `print(msg)`
- `msg` must be convertible to `str`.
- Effective context must provide field `console` assignable to `u64`.
- Lowers to platform console logging with `flags=0` (current implementation).

## 10. Variable-Size Structs (VSS)

Syntax in struct fields:
- `field [ElemType .lenField]`

Constraints:
- Allowed only in `struct` (not `union`).
- `lenField` must reference a previously declared non-dependent integer field in the same struct.
- Once a dependent field appears, all following fields must also be dependent.

Typing/model:
- Dependent field type is pointer-to-element (`*ElemType`).
- Struct becomes variable-size.
- Variable-size classification is transitive through by-value fields in aggregates.

Restrictions:
- Variable-size types are not allowed by value in locals, parameters, or returns.
- Pointer/reference/slice/optional wrappers are allowed.

`sizeof`:
- `sizeof(VarSizeType)` is rejected.
- `sizeof(p)` where `p` points to variable-size type lowers to generated runtime helper.

## 11. Packages, Imports, and Exports

### 11.1 Package model
- Package is inferred from filesystem:
  - Directory package: all `.sl` files in directory.
  - Single-file package mode: one `.sl` file treated as package.
- No `package` keyword.

### 11.2 Imports
Syntax:
- `import "path"`
- `import "path" as alias`
- `import "path" as _`
- `import "path" { Name }`
- `import "path" as alias { Name, Other as Local }`

Resolution:
- Import path is validated after string-literal decoding.
- Path constraints:
  - not empty
  - not absolute (must not start with `/`)
  - no leading/trailing whitespace
  - characters limited to `[A-Za-z0-9_./-]`
  - no empty path segments (`//` rejected)
- Path normalization:
  - `.` segments are removed
  - `..` segments pop one previous segment
  - escaping above loader root is rejected
- If package binding is needed and no explicit `as` is provided, default alias is the last
  normalized path segment.
- Default alias inference is exact; no transformations are applied.
- If inferred alias is not a valid identifier, explicit alias is required.
- `import "path" { ... }` without `as` does not bind a package name.
- `import "path" as _` imports for side effects only and does not bind a package name.
- `import "path" as _ { ... }` is invalid.
- Cyclic imports are rejected.

Special import prefix:
- `import "slang/feature/<name>"` does not resolve to filesystem package.
- Known feature: `optional`.
- Unknown feature names emit a warning.
- Feature imports are path-only and cannot use `as` or `{...}`.
- `import "platform"` resolves to a built-in package named `platform`.
- Built-in `platform` API currently exports `fn exit(status i32)`.

Use:
- Package imports are referenced as `alias.Name` in type names and expressions.
- Named imports bind exported symbols directly into package scope.
- Enum members are not standalone exports:
  - `import "pkg" { Mode }` allows `Mode.A`
  - `import "pkg" { A }` is invalid when `A` is only an enum member
- `import "pkg" { * }` is not supported.
- Unknown alias/symbol is a package check error.

### 11.3 Exports (`pub`)
- `pub` marks exported top-level declaration.
- Exported kinds: same top-level decl kinds (`fn`, `fn{...}`, `struct`, `union`, `enum`, `const`).
- Non-`pub` top-level declarations are package-private.

Package validation performed by `checkpkg`/`genpkg`/`compile`/`run`:
- Duplicate exported symbol (same kind + name) is rejected.
- Exported function declarations must have a definition body in package.
- Public API closure:
  - Exported signatures/fields/const types may reference builtins, exported local types, and
    imported exported types.
  - Public API may not reference private local types.
  - Imported types used in exported API become part of the package's transitive API surface.
    Downstream packages may use those types through the exported package's declarations without
    directly importing the original defining package unless they need to name that package/type
    directly.

Valid transitive-type example:

```sl
// package "a"
pub struct A {
    x int
}

// package "b"
import "a"
pub struct B {
    a a.A
}

// package "c"
import "b"
fn example(v b.B) int {
    return v.a.x
}
```

Entry point:
- `main` does not need `pub`.
- Program entrypoint signature is `fn main()`.
- Exit status is controlled via `platform.exit(status)` when needed.

## 12. Current Lowering-Defined Runtime Semantics

- `switch` currently lowers to `if/else` chains (not C `switch`).
- Optional unwrap lowers to `sl_unwrap(...)` trap-on-null behavior.
- `panic` lowers to platform panic hook (`sl_platform_call`).
- `print` lowers to platform console log with `flags=0`.
- `str` runtime representation is slice-like (`ptr + len`) in current prelude.

## 13. Known Non-Goals / Not Implemented

- No generics/templates/macros.
- No implicit function overloading (`fn name(...)` by signature). Use explicit `fn Group{...};`.
- No block comments.
- No binary integer literal syntax.
- Compound literals are not currently supported.
- Runtime bounds checks for non-constant index/slice operations are analyzed but not currently emitted by C codegen.
