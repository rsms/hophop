# SL Language Specification

This document defines the SL language contract for independent compiler implementations.
It is written to be implementation-oriented and evolution-friendly.

Primary goals:
- remove ambiguity for parser/typechecker/codegen behavior
- separate required language semantics from reference-implementation details
- support incremental evolution with explicit stability tags

See also:
- [spec-glossary.md](spec-glossary.md) for a glossary of terminology use in specification docs
- [spec-conformance.md](spec-conformance.md) for specification conformance matrix

## 1. Conformance Model

### 1.1 Normative terms
- [META-NORM-001][Stable] `MUST`, `MUST NOT`, `SHOULD`, and `MAY` are normative.
- [META-NORM-002][Stable] If normative text conflicts with examples, normative text wins.
- [META-NORM-003][Stable] Sections labeled "Reference Profile" are non-core unless explicitly selected.

### 1.2 Stability tags
- [META-STATUS-001][Stable] Every rule is labeled `Stable`, `Provisional`, or `Draft`.
- [META-STATUS-002][Stable] `Stable` rules are required for Core conformance.
- [META-STATUS-003][Stable] `Provisional` rules are expected for `Reference-slc` compatibility and may evolve.
- [META-STATUS-004][Stable] `Draft` rules are non-implemented proposals and are non-normative.

### 1.3 Conformance profiles
- [META-PROFILE-001][Stable] `Core` profile = all `Stable` rules in this document.
- [META-PROFILE-002][Stable] `Reference-slc` profile = `Core` + `Provisional` + section 14 behavior.
- [META-PROFILE-003][Stable] If behavior is specified only by `Provisional` rules, it is implementation-defined for `Core` conformance.
- [META-PROFILE-004][Stable] When `Stable` and `Provisional` text differ, `Stable` text is authoritative for `Core`.
- [META-PROFILE-005][Stable] For `Reference-slc`, section 14 entries are compatibility notes: unless a rule explicitly says behavior is required, reproducing a listed divergence is optional.

## 2. Lexical Grammar

### 2.1 Source, whitespace, comments
- [LEX-SRC-001][Stable] Source is byte-oriented.
- [LEX-WS-001][Stable] Whitespace bytes: `space`, `tab`, `\r`, `\n`, `\f`, `\v`.
- [LEX-COMMENT-001][Stable] Line comments use `//` to end-of-line.
- [LEX-COMMENT-002][Stable] Block comments use `/*` and `*/`.
- [LEX-COMMENT-003][Stable] Block comments may nest; lexical processing consumes nested pairs until matching outer `*/`.

### 2.2 Identifiers and reserved names
- [LEX-ID-001][Stable] Identifier pattern: `[A-Za-z_][A-Za-z0-9_]*`.
- [LEX-ID-002][Stable] Names beginning with `__sl_` are reserved.
- [LEX-ID-003][Stable] `_` is a hole name, not a normal binding name.
- [LEX-ID-004][Stable] `_` is allowed only in positions that explicitly permit holes:
  - function parameter names
  - local discard declarations: `var _ = expr`, `const _ = expr`
  - side-effect import alias: `import "path" as _`

### 2.3 Keywords
- [LEX-KW-001][Stable] Keywords:
  `import pub struct union enum fn var const type if else for switch case default break continue return defer assert sizeof true false in as null new context`.
- [LEX-KW-002][Stable] `mut` is reserved legacy syntax and MUST be rejected in type positions.

### 2.4 Literals
- [LEX-LIT-001][Stable] Integer literals: decimal or hex (`0x` / `0X`).
- [LEX-LIT-002][Stable] Float literals: decimal with fraction and/or exponent.
- [LEX-LIT-003][Stable] String literals are either interpreted (`"..."`) or raw (`` `...` ``).
- [LEX-LIT-004][Stable] Boolean literals: `true`, `false`.
- [LEX-LIT-005][Stable] Null literal: `null`.
- [LEX-LIT-010][Stable] Rune literals use single quotes (`'...'`) and denote Unicode scalar values.
- [LEX-LIT-006][Provisional] Interpreted-string escapes follow Go-style forms: `\a`, `\b`, `\f`, `\n`, `\r`, `\t`, `\v`, `\\`, `\"`, `\'`, octal `\NNN`, hex `\xNN`, Unicode `\uNNNN`, `\UNNNNNNNN`.
- [LEX-LIT-007][Provisional] Both interpreted and raw string literals may span multiple source lines. Source line endings are normalized to `\n` in decoded string bytes.
- [LEX-LIT-008][Provisional] In interpreted strings, `\` immediately followed by a line break elides that line break. In raw strings, only ``\` `` is treated specially (it encodes a literal backtick).
- [LEX-LIT-009][Provisional] String literals MUST decode to valid UTF-8 byte sequences.
- [LEX-LIT-011][Stable] Rune literals use interpreted-string escape forms (same escape grammar as interpreted strings).
- [LEX-LIT-012][Stable] Rune literals MUST decode to exactly one Unicode scalar value.

### 2.5 Semicolon insertion
- [LEX-SEMI-001][Stable] The formal syntax uses semicolons `";"` as terminators in a number of productions. SL programs may omit most of these semicolons using the following two rules:
  1. A semicolon is inserted at newline when the preceding token can end a statement.
  2. A semicolon is inserted at EOF when the final token can end a statement.
- [LEX-SEMI-002][Stable] Statement-ending tokens are exactly:
  - `IDENT`, `INT`, `FLOAT`, `STRING`, `RUNE`, `TRUE`, `FALSE`, `NULL`
  - `BREAK`, `CONTINUE`, `RETURN`
  - `RPAREN`, `RBRACK`, `RBRACE`
  - `NOT` (postfix unwrap `!` at line end)
  - `CONTEXT` (when tokenized as identifier-like expression operand)
- [LEX-SEMI-003][Stable] In section 3 productions, semicolons are modeled as separators between adjacent declarations/statements in lists.

### 2.6 Directives
- [LEX-DIR-001][Provisional] `@` introduces a directive token sequence at top-level declaration scope.
- [LEX-DIR-002][Provisional] A directive name is an identifier.
- [LEX-DIR-003][Provisional] A directive may be written as `@name` or `@name(...)`.
- [LEX-DIR-004][Provisional] Directive arguments, when present, MUST be zero or more comma-separated SL literals.

## 3. Concrete Syntax

### 3.1 File structure
- [SYN-FILE-001][Stable] `SourceFile` consists of zero or more imports followed by zero or more top-level declarations.
- [SYN-FILE-002][Stable] Imports MUST precede top-level declarations.
- [SYN-FILE-003][Stable] Section 3 grammar is parser grammar over the token stream after lexical processing and semicolon insertion ([LEX-SEMI-001], [LEX-SEMI-002]).

### 3.2 EBNF

```ebnf
SourceFile      = { ImportDecl ";" } { DirectiveRun TopDecl ";" } .
StringLit       = /* lexical string literal; see [LEX-LIT-006] through [LEX-LIT-009] */ .
RuneLit         = /* lexical rune literal; see [LEX-LIT-010] through [LEX-LIT-012] */ .
Literal         = IntLit | FloatLit | StringLit | RuneLit | BoolLit | "null" .
DirectiveRun    = { Directive ";" } .
Directive       = "@" Ident [ "(" [ Literal { "," Literal } [ "," ] ] ")" ] .

ImportDecl      = "import" StringLit [ ImportAlias ] [ ImportSymbols ] .
ImportAlias     = "as" ( Ident | "_" ) .
ImportSymbols   = "{" [ ImportSymbolList ] "}" .
ImportSymbolList = ImportSymbol { ImportSep ImportSymbol } [ ImportSep ] .
ImportSymbol    = Ident [ "as" Ident ] .
ImportSep       = "," | ";" .

TopDecl         = [ "pub" ] ( StructDecl | UnionDecl | EnumDecl | TypeAliasDecl | FnDeclOrDef | TopConstDecl ) .
DeclName        = Ident | "_" .
DeclNameList    = DeclName { "," DeclName } .
TopDeclNameList = Ident { "," Ident } .

TypeParamList   = "[" Ident { "," Ident } "]" .

StructDecl      = "struct" Ident [ TypeParamList ] "{" [ StructFieldDeclList ] "}" .
UnionDecl       = "union" Ident [ TypeParamList ] "{" [ FieldDeclList ] "}" .
EnumDecl        = "enum" Ident [ TypeParamList ] Type "{" [ EnumItemList ] "}" .
TypeAliasDecl   = "type" Ident [ TypeParamList ] Type .
FieldSep        = "," | ";" .
AnonFieldSep    = ";" .

StructFieldDeclList = StructFieldDecl { FieldSep StructFieldDecl } [ FieldSep ] .
FieldDeclList   = FieldDecl { FieldSep FieldDecl } [ FieldSep ] .
AnonFieldDeclList = FieldDecl { AnonFieldSep FieldDecl } [ AnonFieldSep ] .
EnumItemList    = EnumItem { FieldSep EnumItem } [ FieldSep ] .
StructFieldDecl = ( FieldDecl | EmbeddedFieldDecl ) [ FieldDefault ] .
FieldDecl       = Ident { "," Ident } Type .
EmbeddedFieldDecl = TypeName .
FieldDefault    = "=" Expr .
EnumItem        = Ident [ EnumPayload ] [ "=" Expr ] .
EnumPayload     = "{" [ FieldDeclList ] "}" .

FnDeclOrDef     = "fn" FnName [ TypeParamList ] "(" [ ParamList ] ")" [ FnResultClause ]
                [ ContextClause ] [ Block ] .
FnName          = Ident | "sizeof" .
ParamList       = ParamGroup { "," ParamGroup } .
ParamGroup      = "const" ( ( Ident | "_" ) Type | ( Ident | "_" ) "..." Type )
                | ( Ident | "_" ) { "," ( Ident | "_" ) } Type
                | ( Ident | "_" ) "..." Type .
FnResultClause  = Type | "(" FnResultGroup { "," FnResultGroup } ")" .
FnResultGroup   = Type | ( Ident { "," Ident } Type ) .
ContextClause   = "context" Type .

TopConstDecl    = "const" TopDeclNameList ( [ Type ] "=" ExprList ) .
LocalConstDecl  = "const" DeclNameList ( [ Type ] "=" ExprList ) .

Type            = OptionalType | PtrType | RefType | SliceType | ArrayType | VarArrayType
                | FnType | TupleType | AnonStructType | AnonUnionType | TypeName .
OptionalType    = "?" Type .
PtrType         = "*" Type .
RefType         = "&" Type .
SliceType       = "[" Type "]" .
ArrayType       = "[" Type Expr "]" .
VarArrayType    = "[" Type "." Ident "]" .
FnType          = "fn" "(" [ FnTypeParamList ] ")" [ Type ] .
TupleType       = "(" Type "," Type { "," Type } ")" .
FnTypeParamList = FnTypeParam { "," FnTypeParam } .
FnTypeParam     = "const" ( Type | Ident Type | "..." Type )
                | Type | ( Ident { "," Ident } Type ) | "..." Type .
AnonStructType  = [ "struct" ] "{" [ AnonFieldDeclList ] "}" .
AnonUnionType   = "union" "{" [ AnonFieldDeclList ] "}" .
TypeArgList     = "[" Type { "," Type } "]" .
TypeName        = Ident { "." Ident } [ TypeArgList ] .

Block           = "{" [ StmtList ] "}" .
StmtList        = Stmt { ";" Stmt } [ ";" ] .
Stmt            = Block | VarDeclStmt | LocalConstDecl | IfStmt | ForStmt | SwitchStmt
                | ReturnStmt | BreakStmt | ContinueStmt | DeferStmt | AssertStmt
                | ConstBlockStmt | MultiAssignStmt | ExprStmt .

VarDeclStmt     = "var" DeclNameList ( Type [ "=" ExprList ] | "=" ExprList ) .
MultiAssignStmt = ExprList "=" ExprList .
IfStmt          = "if" Expr Block [ "else" ( IfStmt | Block ) ] .
ForStmt         = "for" ( Block | Expr Block | ForClause Block | ForInClause Block ) .
ForClause       = [ ForInit ] ";" [ Expr ] ";" [ Expr ] .
ForInit         = VarDeclStmt | Expr .
ForInClause     = ForInValueBinding "in" Expr
                | ForInKeyBinding "," ForInValueBinding "in" Expr .
ForInKeyBinding = [ "&" ] Ident .
ForInValueBinding = "_" | [ "&" ] Ident .
SwitchStmt      = "switch" [ Expr ] "{" { CaseClause } [ DefaultClause ] "}" .
CaseClause      = "case" CasePattern { "," CasePattern } Block .
CasePattern     = Expr [ "as" Ident ] .
DefaultClause   = "default" Block .
ReturnStmt      = "return" [ ExprList ] .
BreakStmt       = "break" .
ContinueStmt    = "continue" .
DeferStmt       = "defer" ( Block | Stmt ) .
AssertStmt      = "assert" Expr [ "," Expr { "," Expr } ] .
ConstBlockStmt  = "const" Block .
ExprStmt        = Expr .

Expr            = AssignExpr .
AssignExpr      = LogicalOrExpr [ AssignOp AssignExpr ] .
AssignOp        = "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" .
LogicalOrExpr   = LogicalAndExpr { "||" LogicalAndExpr } .
LogicalAndExpr  = BitOrExpr { "&&" BitOrExpr } .
BitOrExpr       = BitXorExpr { "|" BitXorExpr } .
BitXorExpr      = BitAndExpr { "^" BitAndExpr } .
BitAndExpr      = EqualityExpr { "&" EqualityExpr } .
EqualityExpr    = RelExpr { ( "==" | "!=" ) RelExpr } .
RelExpr         = ShiftExpr { ( "<" | ">" | "<=" | ">=" ) ShiftExpr } .
ShiftExpr       = AddExpr { ( "<<" | ">>" ) AddExpr } .
AddExpr         = MulExpr { ( "+" | "-" ) MulExpr } .
MulExpr         = UnaryExpr { ( "*" | "/" | "%" ) UnaryExpr } .
UnaryExpr       = ( ( "+" | "-" | "!" | "*" | "&" ) UnaryExpr ) | PostfixExpr .
PostfixExpr     = PrimaryExpr { PostfixSuffix } .
PostfixSuffix   = CallContextSuffix | IndexSuffix | SelectorSuffix | CastSuffix | UnwrapSuffix .
CallContextSuffix = CallSuffix [ ContextClause ] .
CallSuffix      = "(" [ CallArgList ] ")" .
CallArgList     = CallArg { "," CallArg } .
CallArg         = [ Ident ":" ] Expr [ "..." ] .
ExprList        = Expr { "," Expr } .
ContextClause   = "context" ( "context" | ContextOverlay | Expr ) .
ContextOverlay  = "{" [ ContextBindList ] "}" .
ContextBindList = ContextBind { "," ContextBind } [ "," ] .
ContextBind     = Ident [ ":" Expr ] .
IndexSuffix     = "[" Expr "]" | "[" [ Expr ] ":" [ Expr ] "]" .
SelectorSuffix  = "." Ident .
CastSuffix      = "as" Type .
UnwrapSuffix    = "!" .

PrimaryExpr     = Ident | "context" | IntLit | FloatLit | StringLit | RuneLit | BoolLit | "null"
                | TypeValueExpr
                | CompoundLit | NewExpr | "sizeof" "(" ( Type | Expr ) ")" | "(" Expr ")"
                | TupleExpr .
TypeValueExpr   = "type" Type .
TupleExpr       = "(" Expr "," Expr { "," Expr } ")" .
NewExpr         = "new" ( "[" Type Expr "]" | Type [ "{" [ FieldInitList ] "}" ] ) [ "context" Expr ] .
CompoundLit     = [ TypeName ] "{" [ FieldInitList ] "}" .
FieldInitList   = FieldInit { "," FieldInit } [ "," ] .
FieldInit       = Ident { "." Ident } ":" Expr .
```

### 3.3 Parsing disambiguation rules
- [SYN-DISAMBIG-001][Stable] In function-type parameter lists, parser disambiguation for each comma-delimited segment is:
  1. try `Ident {"," Ident} Type` as a named-parameter group
  2. if that parse fails, backtrack and parse the segment as `Type`
- [SYN-DISAMBIG-002][Stable] Function-type parameter segments are parsed left-to-right with the above backtracking rule.
- [SYN-DISAMBIG-003][Stable] `context` is always lexed as keyword token `CONTEXT`; when accepted as a primary expression it is represented as identifier-like operand, never as a bindable identifier declaration name.
- [SYN-DISAMBIG-004][Provisional] After a declaration name in `struct`, `union`, `enum`, `type`, or `fn`, a following `[` starts a type-parameter list only when the bracket contents parse as identifiers separated by commas. Otherwise the declaration continues without type parameters.
- [SYN-DISAMBIG-005][Stable] In statement context (`Stmt`), leading `{` is parsed unconditionally as `Block`.
- [SYN-DISAMBIG-006][Stable] `ExprStmt` in statement context MUST NOT consume unparenthesized brace-leading compound-literal forms (`{ ... }`). If a compound-literal expression statement is intended, it MUST be parenthesized (`({ ... })`).
- [SYN-DIR-001][Provisional] A top-level directive run attaches to the immediately following top-level declaration.
- [SYN-DIR-002][Provisional] Directives are not standalone declarations and are invalid if no following top-level declaration exists.

Canonical examples:

```sl
fn f() {
    { x = 1 }      // block with one statement: assignment to x
}
```

```sl
fn f() {
    ({ x: 1 })    // expression statement: compound literal value discarded
}
```

```sl
fn f() {
    defer { x = 1 }    // deferred block
}
```

## 4. Declarations, Scope, and Binding

- [DECL-TOP-001][Stable] Top-level declaration kinds: `fn` declarations/definitions, `struct`, `union`, `enum`, `type`, `const`.
- [DECL-TOP-002][Stable] Top-level `var` is invalid.
- [DECL-TOP-003][Stable] `const` MUST have an initializer.
- [DECL-CONST-001][Provisional] `const` initializers MUST be const-evaluable in all scopes (top-level and local).
- [DECL-DIR-001][Provisional] Recognized foreign-linkage directives are:
  - `@c_import("symbol")` on top-level `fn`, `const`, or `var` declarations
  - `@wasm_import("module", "name")` on top-level `fn`, `const`, or `var` declarations
  - `@export("name")` on `pub fn` definitions
- [DECL-DIR-002][Provisional] `@c_import` and `@wasm_import` declare externally provided symbols and therefore require declarations, not definitions.
- [DECL-DIR-003][Provisional] `@export` publishes the annotated function under the requested external name.
- [DECL-DIR-004][Provisional] Foreign-linkage directives are invalid on overloaded functions.
- [DECL-DIR-005][Provisional] Context parameters are invalid on `@c_import` and `@wasm_import` functions.
- [DECL-TOP-004][Stable] `pub` applies to a single following top-level declaration.
- [DECL-HOLE-001][Stable] `_` MUST NOT name top-level symbols, struct/union fields, enum items, or type aliases.
- [DECL-HOLE-002][Stable] Local discard declarations `var _ = expr` and `const _ = expr` are valid statement forms.
- [DECL-HOLE-003][Stable] In declaration and assignment lists, `_` discards the corresponding RHS value and does not create or update a binding.
- [DECL-SCOPE-001][Stable] Scope is lexical by block; nearest declaration wins.
- [DECL-SCOPE-002][Stable] Function/type declarations are collected before body checking (declaration-order independent).

### 4.1 Function declarations and overloading
- [DECL-FN-001][Stable] Multiple declarations for the same function signature are allowed; at most one definition body is allowed.
- [DECL-FN-002][Stable] For a fixed function signature, `context` clauses MUST match exactly.
- [DECL-FN-003][Stable] `context` is not an overload dimension.
- [DECL-FN-004][Stable] `sizeof` is accepted as a declaration name for builtin-signature compatibility, but `sizeof(...)` expression syntax is always the builtin form and is not shadowable by user functions.
- [DECL-FN-005][Stable] Function result clause may be a tuple-style list. Named result groups (`(x, y T)`) provide names for readability only; result names are not local bindings.
- [DECL-FN-006][Provisional] Variadic parameters use `name ...T` and are constrained as follows:
  - at most one variadic parameter per signature
  - variadic parameter MUST be the final parameter
  - grouped-name form is invalid for variadic parameters (`a, b ...T` is invalid)
- [DECL-FN-007][Provisional] For `fn f(...T)`-style declarations where `T != anytype`, the variadic parameter binding inside the body has slice type `[T]`.
- [DECL-FN-008][Provisional] A parameter may be marked `const` (`const name T` or `const name ...T`).
- [DECL-FN-009][Provisional] Grouped-name const parameter form is invalid (`const a, b T` is invalid); write separate const parameters.
- [DECL-FN-010][Provisional] For each argument bound to a `const` parameter, the argument expression MUST be const-evaluable at the call site. For `const` variadic parameters, this applies to each variadic argument and to spread arguments.
- [DECL-FN-011][Provisional] `anytype` is valid only in function parameter positions (function declarations and function-type parameter lists).
- [DECL-FN-012][Provisional] Non-variadic `anytype` parameter slots bind independently to the static type of the corresponding call argument.
- [DECL-FN-013][Provisional] Variadic `...anytype` binds a heterogeneous compile-time pack, not a homogeneous slice.
- [DECL-FN-014][Provisional] Parameters typed as `const_int` or `const_float` MUST be declared with the `const` parameter modifier.

### 4.2 Struct composition and enum member scope
- [DECL-EMBED-001][Stable] `struct` may embed one base field (type-name-only) as first field.
- [DECL-EMBED-002][Stable] Embedded base MUST be a named struct type.
- [DECL-EMBED-003][Stable] Embedded cycles are invalid.
- [DECL-EMBED-004][Stable] Field lookup order is direct fields first, then embedded chain recursively.
- [DECL-ENUM-001][Stable] Enum item names are scoped to enum type; items are not package-global bindings.
- [DECL-ENUM-002][Stable] Enum values are selected as `Enum.Item` or `pkg.Enum.Item`.
- [DECL-ENUM-003][Stable] Enum base type in `enum Name BaseType { ... }` MUST be an integer type.
- [DECL-ENUM-004][Stable] There are no implicit conversions between enum types and integer types; explicit `as` casts are required.
- [DECL-ENUM-005][Stable] Enum variants may define payload fields using struct-field syntax inside `{ ... }` on the variant.
- [DECL-ENUM-006][Stable] Variant explicit tags are allowed for both payload and non-payload variants: `Variant = n` and `Variant{ ... } = n`.
- [DECL-ENUM-007][Stable] Payload variant constructors use compound-literal syntax `Enum.Variant{ ... }`.
- [DECL-ENUM-008][Stable] In payload constructors, omitted payload fields are initialized using the same rules as struct literals.

### 4.3 Namespace model
- [DECL-NS-001][Stable] Type names and value names are distinct lookup spaces.
- [DECL-NS-002][Stable] Function overload sets live in value-name space.
- [DECL-NS-003][Stable] Import bindings (`as` aliases and named imports) MUST NOT collide with any top-level declaration name in the importing package.
- [DECL-NS-004][Stable] Duplicate top-level names are invalid within a namespace, except function overloading and declaration/definition pairing allowed by [DECL-FN-001].

## 5. Type System

### 5.1 Built-in and constructed types
- [TYPE-BUILTIN-001][Stable] Built-ins include `bool`, `str`, `rawptr`, and `type`.
- [TYPE-BUILTIN-002][Stable] Source-level numeric type names are:
  - unsigned integers: `u8`, `u16`, `u32`, `u64`, `uint`
  - signed integers: `i8`, `i16`, `i32`, `i64`, `int`
  - floating point: `f32`, `f64`
  - constant numeric: `const_int`, `const_float`
- [TYPE-BUILTIN-003][Stable] `int` and `uint` are pointer-sized signed/unsigned integers for the target.
- [TYPE-BUILTIN-004][Stable] `Allocator` is a source-level type provided by builtin library declarations (for example `builtin.Allocator` and implicit builtin imports), not a language builtin type.
- [TYPE-BUILTIN-005][Stable] `rune` is a source-level alias type provided by builtin declarations and modeled as `type rune u32`.
- [TYPE-CONSTR-001][Stable] Constructed types: pointers `*T`, references `&T`, arrays `[T N]`, slices `[T]`, dependent arrays `[T .n]`, optionals `?T`, function types, tuple types `(T1, T2, ...)`, anonymous aggregates.
- [TYPE-CONSTR-002][Stable] `[T]` is unsized and MUST NOT be used by value.
- [TYPE-CONSTR-003][Stable] Function type return type defaults to no-value when omitted.
- [TYPE-CONSTR-004][Stable] No-value function return has no source-level type spelling.
- [TYPE-CONSTR-005][Stable] Variable-size-by-value types are invalid in local/param/return/function-type positions.
- [TYPE-CONSTR-006][Stable] Tuple types require at least two element types.
- [TYPE-CONSTR-007][Provisional] In type positions, a top-level `const` name is valid when that constant const-evaluates to a `type` value.
- [TYPE-CONSTR-008][Provisional] `const B = A` (with `A` a type value) is a non-distinct type alias; `B` resolves to the same type as `A`. Use `type B A` for a distinct named type.
- [TYPE-CONSTR-009][Provisional] Function types may be variadic by marking the final parameter as `...T` (e.g. `fn(...i32) i32`).
- [TYPE-CONSTR-010][Provisional] Variadic function types are distinct from non-variadic function types with trailing slice parameters.
- [TYPE-CONSTR-011][Provisional] Function types include parameter `const` markers in type identity; for example, `fn(i32)` and `fn(const i32)` are distinct.
- [TYPE-CONSTR-012][Provisional] `anytype` participates in function type identity (for example, `fn(anytype)` is distinct from `fn(i32)`).
- [TYPE-CONSTR-013][Provisional] Variadic function types ending in `...anytype` are distinct from variadic function types ending in concrete `...T`.
- [TYPE-CONSTR-014][Provisional] `const_int` and `const_float` type names are valid only in function return types, `const` parameter types, `const` declaration explicit types, and `as` cast targets.
- [TYPE-GENERIC-001][Provisional] Named `struct`, `union`, `enum`, `type`, and `fn` declarations MAY declare type parameters with `[...]` after the declaration name.
- [TYPE-GENERIC-002][Provisional] Type parameters denote compile-time values of builtin metatype `type` within the generic declaration body.
- [TYPE-GENERIC-003][Provisional] Generic named types are instantiated only in type positions, using `TypeName[TypeArgs...]`.
- [TYPE-GENERIC-004][Provisional] Generic named types used in explicit compound-literal type prefixes MUST include all required type arguments; bare `Vector{ ... }` is invalid when `Vector` is generic.
- [TYPE-GENERIC-005][Provisional] Generic function calls do not admit explicit type-argument syntax. `f[T](x)` is not a generic call form.
- [TYPE-GENERIC-006][Provisional] A generic function declaration is invalid if it has zero ordinary parameters and any declared type parameter appears in the return type.

### 5.1.1 Textual types (`str` and `rune`)
- [TYPE-TEXT-001][Stable] `str` denotes textual byte sequences constrained to valid UTF-8.
- [TYPE-TEXT-002][Stable] String literals MUST decode to valid UTF-8 and therefore produce well-formed `str` data ([LEX-LIT-003], [LEX-LIT-009]).
- [TYPE-TEXT-003][Stable] `str` operations are byte-oriented by default: `len(s)` reports byte length, not Unicode scalar-value count ([BI-LEN-001], [BI-LEN-002]).
- [TYPE-TEXT-004][Stable] `str` is for textual data; arbitrary binary payloads SHOULD use `[u8]` families (`[u8 N]`, `*[u8]`, `&[u8]`) instead of `str`.
- [TYPE-TEXT-005][Stable] `rune` denotes one Unicode scalar value and is modeled as builtin alias `type rune u32` ([TYPE-BUILTIN-005], [LEX-LIT-010], [LEX-LIT-012]).
- [TYPE-TEXT-006][Stable] Rune literals infer type `const_int`; values typed as `rune` can still implicitly convert to integer destinations only in the in-range const-evaluated case ([TYPE-INFER-004], [TYPE-ASSIGN-008]).
- [TYPE-TEXT-007][Stable] C interop over string bytes uses `cstr(s)`, which exposes `str` storage as `&u8` with the lifetime/termination guarantees in [BI-CSTR-001] through [BI-CSTR-005].

### 5.2 Mutability model
- [TYPE-MUT-001][Stable] `*T` is writable reference-like access to `T`.
- [TYPE-MUT-002][Stable] `&T` is read-only reference-like access to `T`.
- [TYPE-MUT-003][Stable] Slice mutability follows wrapper mutability:
  - `*[T]` / `*[T N]` are writable slice views
  - `&[T]` / `&[T N]` are read-only slice views
- [TYPE-MUT-004][Stable] There is no source syntax for `mut` references or slices.

### 5.3 Optional types
- [TYPE-OPT-001][Stable] `?T` accepts any well-formed `T`.
- [TYPE-OPT-002][Stable] `null` is assignable only to optional types and `rawptr`.
- [TYPE-OPT-003][Stable] Implicit lift `T -> ?T` is allowed.
- [TYPE-OPT-004][Stable] `?T -> T` is not implicit; unwrap or narrowing is required.
- [TYPE-OPT-005][Stable] Flow narrowing for optionals is supported for local identifiers (including parameters):
  - `if x { ... }` narrows `x` to `T` in `then`, `null` in `else`
  - `x == null`, `x != null`, `null == x`, `null != x`
  - `if x == null { ... } else { ... }` narrows `x` to `null` in `then`, `T` in `else`
  - `if x != null { ... } else { ... }` narrows `x` to `T` in `then`, `null` in `else`
  - continuation narrowing after `if` without `else` applies only when the `then` branch terminates (`return`, `break`, or `continue`)
- [TYPE-OPT-006][Provisional] Feature imports `slang/feature/optional` and `feature/optional` are recognized but not required to enable `?` in `Reference-slc`.

### 5.4 Assignability and coercion
- [TYPE-ASSIGN-001][Stable] Assignment requires exact type match except explicit implicit-conversion rules.
- [TYPE-ASSIGN-002][Stable] Constant numeric expressions (`const_int`, `const_float`) convert to compatible concrete numeric types subject to range/precision checks.
- [TYPE-ASSIGN-003][Stable] `*T -> &T` is allowed; read-only to writable is not.
- [TYPE-ASSIGN-004][Stable] Array/slice view conversions:
  - `[S N] -> &[S]`
  - `[S N] -> *[S]` when source is mutable lvalue
  - `&[S N] -> &[S]`
  - `*[S N] -> *[S]`
  - `*[S] -> &[S]`
- [TYPE-ASSIGN-005][Stable] Embedded-base upcasts are allowed by value and pointer/reference forms.
- [TYPE-ASSIGN-006][Stable] Alias conversion direction is nominal one-way: `Alias -> Target` implicit, `Target -> Alias` not implicit.
- [TYPE-ASSIGN-007][Stable] No implicit numeric widening between concrete numeric types.
- [TYPE-ASSIGN-008][Stable] Rune expressions (`rune`) can implicitly convert to integer destinations only when const evaluation produces an in-range value.
- [TYPE-ASSIGN-009][Stable] Any typed value is assignable to `_`.
- [TYPE-ASSIGN-010][Stable] Assigning a const-numeric value to `_` first applies implicit defaulting (`const_int -> int`, `const_float -> f64`, `untyped_bool -> bool`).

### 5.5 Inference and zero values
- [TYPE-INFER-001][Stable] `var x = expr` infers from `expr` after concretization; `const x = expr` preserves const-numeric inference.
- [TYPE-INFER-002][Stable] For `var` inference, `const_int` defaults to `int` and `const_float` defaults to `f64`.
- [TYPE-INFER-003][Stable] Inference from `null` or no-value expressions is invalid.
- [TYPE-INFER-004][Stable] Rune literals infer type `const_int`.
- [TYPE-ZERO-001][Stable] `var x T` zero-initializes `x`, except direct pointer/reference locals (`*T` and `&T`) are uninitialized until assigned and cannot be read, dereferenced, returned, passed, or otherwise used before definite assignment.
- [TYPE-ZERO-002][Stable] For enum types, `var x EnumType` is valid only when one variant has effective tag value `0`.
- [TYPE-ZERO-003][Stable] Definite assignment for direct pointer/reference locals is flow-sensitive across `if` and `switch`; a value assigned only on some continuing control-flow paths is considered may-be-uninitialized. Assignments inside loop bodies do not definitely initialize the local after the loop. Parameters and `for ... in` bindings are initialized on entry.
- [TYPE-ZERO-004][Stable] Top-level variables of type `*T` or `&T` require an explicit initializer. `rawptr` keeps normal zero initialization and is not tracked by pointer/reference definite assignment.
- [TYPE-INFER-005][Stable] Grouped declarations infer and/or check per position (`var a, b = 1, 2`; `var a, b T = x, y`). Initializer arity must match declaration arity, except a single tuple-typed RHS may be decomposed positionally.

## 6. Expressions and Operators

- [EXPR-OP-001][Stable] Operator precedence is: postfix, unary, multiplicative/shift/bit-and, additive/bit-or/bit-xor, relational/equality, logical-and, logical-or, assignment. This follows Go's operator precedence for shared binary operators; assignment remains SL-specific and lowest.
- [EXPR-UNARY-001][Stable] Unary `+`/`-` require numeric operands; unary `!` requires bool.
- [EXPR-UNARY-002][Stable] Unary `*` dereferences pointer/reference; unary `&` forms read-only references.
- [EXPR-ASSIGN-001][Stable] Assignment LHS MUST be assignable (identifier/index/non-dependent field/dereference of writable location).
- [EXPR-ASSIGN-002][Stable] Compound assignment requires assignable LHS and numeric LHS type.
- [EXPR-ASSIGN-003][Stable] Multi-assignment (`lhs1, lhs2, ... = rhs1, rhs2, ...`) requires equal arity, except a single tuple-typed RHS may be decomposed positionally. RHS expressions are evaluated before stores; then stores apply left-to-right.
- [EXPR-ASSIGN-004][Provisional] Assigning to a `const` binding is invalid.
- [EXPR-CMP-001][Stable] Equality/ordering require coercion to a common comparable/ordered type, except optional-null and pointer/rawptr-vs-`null` equality special-cases.
- [EXPR-CAST-001][Stable] `as` is explicit cast syntax.
- [EXPR-CAST-002][Stable] A cast expression is well-typed only when source expression typing succeeds, target type resolution succeeds, and the cast pair is explicitly permitted.
- [EXPR-CAST-003][Stable] `rawptr` may be created only from `null`, another `rawptr`, or an explicit cast from a pointer/reference type; casts from `rawptr` are permitted only to `rawptr` or pointer/reference types.
- [EXPR-CAST-004][Stable] Direct casts from `null` are permitted only to optional types and `rawptr`; typed null pointers/references require an explicit raw pointer bridge such as `(null as rawptr) as *T`.
- [EXPR-UNWRAP-001][Stable] `x!` requires `x : ?T` and yields `T`.
- [EXPR-UNWRAP-002][Stable] Unwrapping `null` is a runtime trap (panic), never undefined behavior.
- [EXPR-ADD-001][Provisional] String `+` currently supports compile-time concatenation only for non-parenthesized literal chains (e.g. `"a" + "b" + "c"`). Other string `+` forms are invalid in `Reference-slc`.

### 6.1 Comparable and ordered types
- [EXPR-COMMON-001][Stable] Binary-operation common-type selection is:
  1. if `leftType == rightType`, use that type
  2. if left is const numeric and right is concrete typed and `left` is assignable to right, use right
  3. if right is const numeric and left is concrete typed and `right` is assignable to left, use left
  4. if pair is (`const_int`, `const_float`) or reversed, use `const_float`
  5. otherwise there is no common type
- [EXPR-COMMON-002][Stable] Equality/ordering comparisons that are not handled by optional-null special case or comparison hooks MUST use [EXPR-COMMON-001].
- [EXPR-CMPSET-001][Stable] Comparable types are:
  - `bool` and numeric types
  - `type` values
  - `rawptr`
  - string-like values (`str`, `*str`, `&str`)
  - pointers/references
  - arrays/slices whose element type is comparable
  - enum types
  - struct/union types whose fields are all comparable
  - optional types whose base type is comparable
- [EXPR-CMPSET-002][Stable] Ordered types are:
  - numeric types
  - `rawptr`
  - string-like values (`str`, `*str`, `&str`)
  - pointers/references
  - arrays/slices whose element type is ordered
  - enum types
  - optional types whose base type is ordered
- [EXPR-CMPSEM-001][Stable] Operational comparison semantics are:
  - string-like: content-based bytewise equality/order
  - `rawptr`: identity equality; ordering by pointer-address order
  - pointers/references (and pointer-vs-`null`): identity equality; ordering by pointer-address order
  - arrays/slices: bytewise sequence equality/order over `len * sizeof(element)`
  - `struct`/`union` equality: bytewise object-representation equality
  - tagged-enum equality: bytewise value equality (tag and payload)
  - tagged-enum ordering: by tag value only
  - other scalar comparable/ordered types: native scalar compare after coercion
- [EXPR-CMPSEM-002][Stable] If a visible comparison hook `__equal(lhs, rhs)` or `__order(lhs, rhs)` is a best overload match, hook result semantics override [EXPR-CMPSEM-001] for that operation.
- [EXPR-CMPHOOK-001][Stable] Comparison-hook declarations are:
  - `__equal(a, b T) bool`-shape: exactly 2 parameters, no `context` clause, return type `bool`
  - `__order(a, b T) int`-shape: exactly 2 parameters, no `context` clause, return type `int`
- [EXPR-CMPHOOK-002][Stable] For both hook kinds, parameter-2 type MUST be assignable to parameter-1 type.
- [EXPR-CMPHOOK-003][Stable] Operator mapping when hook resolution succeeds:
  - `==` uses `__equal(lhs, rhs)`
  - `!=` uses logical negation of `__equal(lhs, rhs)`
  - `<`, `<=`, `>`, `>=` use `__order(lhs, rhs)` compared against `0` with corresponding relation.
- [EXPR-CMPHOOK-004][Stable] Comparison hooks MUST be pure:
  - MUST NOT use `context`
  - MUST NOT call functions/builtins
  - MUST NOT allocate (`new`)
  - MUST NOT read top-level `var`/`const` symbols

### 6.2 Selector call sugar and overload resolution
- [EXPR-SUGAR-001][Stable] For call form only, `recv.f(args...)` may resolve as `f(recv, args...)`.
- [EXPR-SUGAR-002][Stable] Actual field lookup has precedence over selector-call sugar.
- [EXPR-SUGAR-003][Stable] Sugar is call-only; `recv.f` alone does not produce callable lowering sugar.
- [EXPR-SUGAR-004][Stable] Overload resolution is deterministic by conversion-cost ranking; ties are ambiguous-call errors.
- [EXPR-SUGAR-005][Stable] Auto-ref for receiver-first overload matching is allowed when required by candidate signatures.
- [EXPR-SUGAR-006][Stable] Writable-reference parameters MUST reject compound temporaries.
- [EXPR-SUGAR-007][Stable] Candidate ranking uses lexicographic per-argument conversion cost, then total-cost tie-break.
- [EXPR-SUGAR-008][Stable] Conversion costs are:
  - `0`: exact type match
  - `1`: direct non-exact implicit conversion (for example alias peel step, mutability relaxation)
  - `2+`: embedded upcast conversions (`2 + ancestry_distance - 1`)
  - `3`: const-numeric concretization conversion
  - `4`: optional lift `T -> ?T`
- [EXPR-SUGAR-009][Stable] Multi-step conversion-cost composition rules are:
  - alias-source peeling is recursive and additive (`cost(dst <- alias(src)) = cost(dst <- srcBase) + 1`)
  - optional-to-optional conversion recurses on base (`cost(?A <- ?B) = cost(A <- B)`)
  - all non-recursive assignable non-exact conversions default to cost `1` unless matched by a more specific cost rule above.
- [EXPR-SUGAR-010][Provisional] For variadic signature `f(p1, ..., pn, v ...T)`, call shapes are:
  - fixed-only: `f(a1, ..., an)` (empty variadic tail)
  - explicit tail: `f(a1, ..., an, e1, ..., ek)` where each `ei` is assignable to `T`
  - spread tail: `f(a1, ..., an, s...)` where `s` is assignable to `[T]`
- [EXPR-SUGAR-011][Provisional] Spread marker `...` is valid only on the final call argument.
- [EXPR-SUGAR-012][Provisional] In spread form, the spread argument MUST be unlabeled and the call MUST provide exactly one spread argument for the variadic tail.
- [EXPR-SUGAR-013][Provisional] In explicit-tail form, variadic-tail arguments are positional only; explicit named arguments are not allowed in the variadic tail.
- [EXPR-SUGAR-014][Provisional] Named arguments (when present) participate only in fixed-parameter mapping; variadic-tail mapping is positional.
- [EXPR-SUGAR-015][Provisional] For variadic signature `f(...anytype)`, explicit-tail arguments form a heterogeneous pack with per-element static type preservation.
- [EXPR-SUGAR-016][Provisional] For `...anytype` pack bindings, `len(args)` is valid.
  - `args[i]` with const-evaluable in-bounds integer index yields the indexed element type.
  - `args[i]` with non-const index is allowed in expected-type contexts (e.g. cast/coercion, typed call arguments, typed assignment) and in `typeof(args[i])`; these lower to runtime per-element dispatch over the instantiated pack.
- [EXPR-SUGAR-017][Provisional] Spread into `...anytype` requires an `anytype` pack value; slice/array spread into `...anytype` is invalid.

### 6.3 Indexing and slicing
- [EXPR-INDEX-001][Provisional] In const-evaluated execution paths, element indexing `x[i]` over const-evaluable string/slice-like values produces a const byte value when `i` is const-evaluable and in bounds.
- [EXPR-SLICE-001][Provisional] Slice-range expressions over sequence-like values preserve the source sequence family and mutability/view shape rather than converting through a different container kind.
  - examples: `*[T][a:b] -> *[T]`, `&[T][a:b] -> &[T]`, `(*str)[a:b] -> *str`, `(&str)[a:b] -> &str`

### 6.3.1 Type values in expression context
- [EXPR-TYPEVALUE-001][Provisional] `type T` forms a type-value expression of metatype `type`.
- [EXPR-TYPEVALUE-002][Provisional] `type` prefix is required when an expression-context type value would otherwise collide with ordinary postfix expression grammar, including instantiated generic named types (`type Vector[i32]`) and constructed types (`type &[i32]`).
- [EXPR-TYPEVALUE-003][Provisional] Simple builtin or named type value expressions that are already unambiguous, such as `i64`, remain valid without the `type` prefix.

### 6.4 Compound literals
- [EXPR-COMPOUND-001][Stable] Compound literals are named-field only.
- [EXPR-COMPOUND-002][Stable] Field names may be dotted (`a.b.c: ...`).
- [EXPR-COMPOUND-003][Stable] Inferred `{ ... }` without explicit type requires expected aggregate type context or anonymous-struct inference.
- [EXPR-COMPOUND-004][Stable] Anonymous-struct inference from `{ field: expr, ... }` uses field names and concretized field value types.
- [EXPR-COMPOUND-005][Stable] Duplicate field initializer paths in the same literal are invalid.
- [EXPR-COMPOUND-006][Stable] Omitted fields are allowed:
  - struct fields without explicit initializer and without field-default evaluate to zero-value
  - struct fields with declaration default evaluate to their default expression unless suppressed by explicit direct-field initializer
  - embedded struct fields may have declaration defaults; promoted fields from the embedded value are visible to later defaults
  - union literals may initialize at most one field explicitly; with zero explicit fields the union is zero-initialized.
- [EXPR-COMPOUND-007][Stable] Explicit initializers take precedence over defaults for the same direct field.
- [EXPR-COMPOUND-008][Stable] Default-suppression matching is by direct field name; dotted subfield initializer paths and promoted-field initializer paths do not suppress defaults of containing direct fields. These explicit initializers are applied over the containing default value.
- [EXPR-COMPOUND-009][Stable] Tagged-enum payload construction is via explicit variant type in the literal type position: `Enum.Variant{ ... }`.
- [EXPR-COMPOUND-010][Stable] `Enum.Variant{ ... }` type-checks against payload fields of that exact variant only.

## 7. Statements and Control Flow

- [STMT-IF-001][Stable] `if` condition MUST be bool or optional.
- [STMT-IF-002][Provisional] In const-evaluated statement contexts and template-instance function bodies, if-condition expressions that are const-evaluable booleans specialize branch checking: only the taken branch is required to typecheck.
- [STMT-FOR-001][Stable] `for` forms: infinite block, condition form, C-style `init; cond; post`, and `for ... in` forms.
- [STMT-FOR-002][Stable] `for` condition (if present) MUST be bool.
- [STMT-FOR-003][Stable] Variables declared in `for` initializer are scoped to the entire loop (condition, post, and body) and are not visible after the loop.
- [STMT-FOR-004][Provisional] `for ... in` source expression MUST be iterable by either:
  - sequence path: supports `len(x)` and indexing `x[i]` (SLP-29), or
  - iterator protocol path: provides `__iterator(x)` and matching `next_*` overloads for the loop form (SLP-30).
- [STMT-FOR-005][Provisional] `for ... in` source expression is evaluated once before loop iteration.
- [STMT-FOR-006][Provisional] In `for key, value in x`, the key for sequence sources is synthetic `uint` index; `&key` is invalid for synthetic keys.
- [STMT-FOR-007][Provisional] Value binding modes in `for ... in`:
  - `value`: by-value capture
  - `&value`: by-reference capture with `&x[i]` semantics (mutable source -> `*T`, immutable source -> `&T`)
  - `_`: discard capture (no value binding introduced)
- [STMT-FOR-008][Provisional] `for ... in` key/value bindings are body-scope locals.
- [STMT-FOR-009][Provisional] Iterator-protocol binding-mode mapping:
  - `for value in x` prefers `next_value(it *Iter) ?(&T|*T)`; if unavailable, uses `next_key_and_value(it *Iter) ?*Pair` and binds from pair element 1 by value.
  - `for &value in x` prefers `next_value(it *Iter) ?(&T|*T)`; if unavailable, uses `next_key_and_value(it *Iter) ?*Pair` and binds from pair element 1 by reference.
  - `for key, value in x` requires `next_key_and_value(it *Iter) ?*Pair` and binds key from pair element 0 and value from pair element 1.
  - `for key, _ in x` prefers `next_key(it *Iter) ?(&K|*K)`; if unavailable, uses `next_key_and_value(it *Iter) ?*Pair` and binds key from pair element 0.
- [STMT-FOR-010][Provisional] Iterator types may implement only a subset of binding modes. Using an unsupported mode is a compile-time error.
- [STMT-FOR-011][Provisional] For discard capture (`for _ in x`), iterator protocol uses the value-binding path and discards produced values.
- [STMT-FOR-012][Provisional] Iterator-protocol `__iterator` overload resolution follows regular type-function first-argument matching:
  - first attempt with source expression type `typeof(x)`
  - if no overload matches and source expression is assignable, retry with autoref first argument
- [STMT-SWITCH-001][Stable] `switch` supports expression-switch and condition-switch.
- [STMT-SWITCH-002][Stable] At most one `default` clause.
- [STMT-SWITCH-003][Stable] No fallthrough.
- [STMT-SWITCH-004][Stable] Expression-switch case labels must be assignable to subject type; condition-switch labels must be bool.
- [STMT-SWITCH-005][Stable] Case labels are tested left-to-right and first matching case body executes.
- [STMT-CONST-001][Provisional] `const { ... }` executes its block in compile-time evaluation context at call sites.
- [STMT-CONST-002][Provisional] `const { ... }` failure to evaluate at compile time is a compile-time error.
- [STMT-SWITCH-006][Stable] Duplicate case labels are not required to be diagnosed statically.
- [STMT-SWITCH-007][Stable] Expression-switch semantics are defined as if subject expression is evaluated once before case-label matching.
- [STMT-SWITCH-008][Stable] For finite-domain subjects (`bool`, `enum`), switches MUST be exhaustive unless a `default` clause is present.
- [STMT-SWITCH-009][Stable] Case pattern `Enum.Variant as name` introduces `name` in case-body scope and narrows it to that variant.
- [STMT-SWITCH-010][Stable] Subject-identifier narrowing for enum payload field access is switch-only: within a single-variant case body, the subject identifier is narrowed to that variant.
- [STMT-SWITCH-011][Stable] Payload field selection requires a narrowed value (subject identifier narrowed by switch case, or `as` alias).
- [STMT-CTRL-001][Stable] `break` valid inside `for`/`switch`; `continue` only inside `for`.
- [STMT-RETURN-001][Stable] If a function has no return type, `return` must not provide values.
- [STMT-RETURN-002][Stable] If a function has a non-tuple return type, `return` must provide exactly one value assignable to that type.
- [STMT-RETURN-003][Stable] If a function has a tuple return type, `return` must provide exactly one value per tuple element, each assignable by position.
- [STMT-DEFER-001][Stable] `defer` supports statement or block, executes LIFO on scope exit.
- [STMT-DEFER-002][Stable] Defers are guaranteed on structured scope exits (normal fallthrough, `return`, `break`, `continue`), but not guaranteed after runtime traps/aborts (`panic`, failed `assert`, process termination).
- [STMT-ASSERT-001][Stable] First assert argument MUST be bool; if format argument exists it MUST be `str`-assignable.
- [STMT-ASSERT-002][Stable] Additional assert arguments are accepted after format expression but formatting semantics are implementation-defined.
- [STMT-ASSERT-003][Stable] Argument evaluation order for assert formatting arguments is implementation-defined.

## 8. Contexts and Capabilities

- [CTX-DECL-001][Stable] Function context clause syntax is `context Type` where `Type` is full type grammar (named or anonymous struct-compatible type).
- [CTX-DECL-002][Stable] A function with context clause has an implicit local identifier `context` of type `*ContextType`.
- [CTX-DECL-003][Stable] Context requirement types are struct-compatible: after alias resolution they must be named structs or anonymous structs.
- [CTX-DECL-004][Stable] `context` is a reserved contextual identifier expression:
  - valid as primary expression operand where a current function context or implicit main root context exists
  - invalid as a declaration/binding name
  - unresolved (unknown symbol) outside a context-bearing scope
- [CTX-CALL-001][Stable] Calls without `context` clause forward effective current context automatically.
- [CTX-CALL-002][Stable] `context context` is explicit pass-through and equivalent to omission.
- [CTX-CALL-003][Stable] `context { ... }` creates call-local context overlay.
- [CTX-CALL-003A][Stable] `context expr` passes an explicit context expression.
- [CTX-CALL-004][Stable] Overlay bind shorthand `name` means `name = context.name`.
- [CTX-CALL-005][Stable] Overlay duplicate field names are invalid.
- [CTX-CALL-006][Stable] Callee context requirements are structural-by-field: each required field name/type must be provided by effective context.
- [CTX-CALL-007][Stable] Overlay bindings take precedence over forwarded ambient fields with the same name.

## 9. Built-in Forms and Functions

### 9.1 `len(x)`
- [BI-LEN-001][Stable] Valid argument families: `str`, arrays, slices, pointers/references to arrays/slices.
- [BI-LEN-002][Stable] Return type is `uint`.
- [BI-LEN-003][Stable] Selector-call sugar `x.len()` is equivalent when no field `len` shadows it.

### 9.2 `cstr(s)`
- [BI-CSTR-001][Stable] Argument MUST be `str`-assignable.
- [BI-CSTR-002][Stable] Return type is `&u8`.
- [BI-CSTR-003][Stable] Selector-call sugar `x.cstr()` is supported.
- [BI-CSTR-004][Stable] Returned pointer references the source string byte storage and remains valid while that storage remains valid.
- [BI-CSTR-005][Stable] For non-null string values the returned byte sequence is NUL-terminated.

### 9.3 `copy(dst, src)`
- [BI-COPY-001][Stable] `copy(dst, src)` copies `min(len(dst), len(src))` elements from `src` into `dst`.
- [BI-COPY-002][Stable] Source and destination ranges may overlap; semantics are equivalent to elementwise `memmove` over copied bytes.
- [BI-COPY-003][Stable] Return type is `uint`, and equals the number of elements copied.
- [BI-COPY-004][Stable] `dst` MUST be writable sequence-like and `src` MUST be readable sequence-like.
- [BI-COPY-005][Stable] String interoperability:
  - `copy(*str, &str)` is valid.
  - `copy(*[u8], &str)` is valid.
  - `copy(*str, &[u8])` is invalid unless `src` is explicitly cast to `&str`.

### 9.4 `new`
- [BI-NEW-001][Stable] Forms:
  - `new T`
  - `new T{...}`
  - `new [T n]`
  - each with optional `context allocExpr`
- [BI-NEW-002][Stable] Without explicit allocator, effective context MUST provide `mem` compatible with `*Allocator`.
- [BI-NEW-002A][Stable] The explicit allocator, or effective context `mem`, MUST be non-null and have a valid allocator implementation.
- [BI-NEW-003][Stable] `n` in `new [T n]` MUST be integer-typed; constant negative values are invalid.
- [BI-NEW-004][Stable] Variable-size element types require initializer in non-count form.
- [BI-NEW-005][Stable] Static result typing:
  - `new T` -> `*T`
  - `new [T n]` -> `*[T N]` when `n` is constant positive `N`, else `*[T]`
- [BI-NEW-005A][Stable] `new [T 0]` has type `*[T]` (runtime-length slice pointer form).
- [BI-NEW-006][Provisional] In `Reference-slc`, codegen may insert implicit null-trap unwrap when coercing `new` into non-optional pointer destinations.

### 9.5 `concat(a, b)` and `free(...)`
- [BI-CONCAT-001][Stable] `concat(a, b)` requires both args `str`-assignable and context `mem`; returns `*str`.
- [BI-FREE-001][Stable] `free(value)` uses context allocator; `free(alloc, value)` uses explicit allocator.
- [BI-FREE-002][Stable] Method sugar `alloc.free(value)` is supported.

### 9.6 `panic(msg)`
- [BI-PANIC-001][Stable] Argument MUST be `str`-assignable.
- [BI-PANIC-002][Stable] Returns no value.

### 9.7 `sizeof`
- [BI-SIZEOF-001][Stable] `sizeof(Type)` and `sizeof(expr)` forms are supported.
- [BI-SIZEOF-002][Stable] Result type is `uint`.
- [BI-SIZEOF-003][Stable] Unsized and variable-size-by-value type operands are invalid in `sizeof(Type)`.

### 9.8 `print(msg)`
- [BI-PRINT-001][Stable] Argument MUST be `str`-assignable.
- [BI-PRINT-002][Stable] Effective context MUST provide field `log`.
- [BI-PRINT-003][Stable] Core type checking imposes no additional static shape/type requirement on `log` beyond field presence.
- [BI-PRINT-004][Provisional] `Reference-slc` currently validates `log` field presence at typecheck time and may rely on backend coercion at codegen time for concrete logger compatibility.

### 9.9 `fmt(format, args...)`
- [BI-FMT-001][Provisional] `fmt` requires at least one argument; the first argument MUST be `str`-assignable.
- [BI-FMT-002][Provisional] `fmt` requires effective context field `mem` compatible with `*Allocator`.
- [BI-FMT-003][Provisional] `fmt` returns `*str`.
- [BI-FMT-004][Provisional] In v1, supported placeholders are `{i}` (integer) and `{r}` (reflective).
- [BI-FMT-005][Provisional] `{{` and `}}` represent literal braces.
- [BI-FMT-006][Provisional] If format is const-evaluable, placeholder count and argument compatibility are checked at typecheck time.

### 9.10 `typeof(x)`
- [BI-TYPEOF-001][Provisional] `typeof(x)` requires exactly one argument.
- [BI-TYPEOF-002][Provisional] `typeof(x)` returns a value of builtin metatype `type`.
- [BI-TYPEOF-003][Provisional] The returned type value represents the static type of `x`.
- [BI-TYPEOF-004][Provisional] For type-name operands, `typeof(T)` is `type` (because type names are value expressions whose type is `type`).

### 9.11 `kind(...)` and `base(...)`
- [BI-REFLECT-001][Provisional] `kind(t)` and `t.kind()` require one `type` operand and return `reflect.Kind` (fallback `u8` when `reflect.Kind` is unavailable).
- [BI-REFLECT-002][Provisional] `base(t)` and `t.base()` require one `type` operand and return `type`.
- [BI-REFLECT-003][Provisional] `base(...)` is valid only for alias type values; non-alias operands are a type error.
- [BI-REFLECT-004][Provisional] `is_alias(t)` and `t.is_alias()` require one `type` operand and return `bool`.
- [BI-REFLECT-005][Provisional] `type_name(t)` and `t.type_name()` require one `type` operand and return `&str`.
- [BI-REFLECT-006][Provisional] `ptr(t)` requires one `type` operand and returns `type` representing `*t`.
- [BI-REFLECT-007][Provisional] `slice(t)` requires one `type` operand and returns `type` representing `[t]`.
- [BI-REFLECT-008][Provisional] `array(t, n)` requires `type` + integer operands and returns `type` representing `[t n]`; `n` must be const-evaluable and in `0..UINT32_MAX` when materialized.
- [BI-REFLECT-009][Provisional] `span_of(x)` and `reflect.span_of(x)` return `reflect.Span` for operand `x`.

### 9.12 `compiler.error*` / `compiler.warn*`
- [BI-CONSTEVAL-DIAG-001][Provisional] `compiler.error(message)` and `compiler.warn(message)` require one `str`-assignable argument.
- [BI-CONSTEVAL-DIAG-002][Provisional] `compiler.error_at(span, message)` and `compiler.warn_at(span, message)` require `reflect.Span` + `str`-assignable arguments.
- [BI-CONSTEVAL-DIAG-003][Provisional] Calls are valid in ordinary code and in consteval.
- [BI-CONSTEVAL-DIAG-004][Provisional] Outside consteval, diagnostics are emitted only on compile-time-proven execution paths.
- [BI-CONSTEVAL-DIAG-005][Provisional] For emitted diagnostics, message must be const-evaluable `&str`; `_at` forms also require a valid const-evaluable `reflect.Span`.

## 10. Variable-Size Structs (VSS)

- [VSS-SYNTAX-001][Stable] Dependent field syntax: `field [ElemType .lenField]`.
- [VSS-SYNTAX-002][Stable] Dependent fields are allowed only in `struct`.
- [VSS-SYNTAX-003][Stable] `lenField` MUST refer to a previously declared non-dependent integer field.
- [VSS-SYNTAX-004][Stable] After first dependent field, all following fields MUST be dependent.
- [VSS-TYPE-001][Stable] Dependent field static type is pointer-to-element (`*ElemType`).
- [VSS-TYPE-002][Stable] VSS types are invalid by value in locals/params/returns.
- [VSS-SIZEOF-001][Stable] `sizeof(VSSType)` is invalid; pointer/reference value forms are valid.

## 11. Packages, Imports, and Exports

### 11.1 Package model
- [PKG-MODEL-001][Stable] Package identity is filesystem-derived (directory package or single-file package mode).
- [PKG-MODEL-002][Stable] There is no `package` declaration keyword.

### 11.2 Imports
- [PKG-IMPORT-001][Stable] Import forms:
  - `import "path"`
  - `import "path" as alias`
  - `import "path" as _`
  - `import "path" { Name, Other as Local }`
  - combinations with alias + named imports
- [PKG-IMPORT-002][Stable] Path constraints: non-empty, relative, normalized, no root escape.
- [PKG-IMPORT-002A][Stable] Normalization removes `.` segments, resolves `..` by popping one segment, and rejects traversal above loader root.
- [PKG-IMPORT-002B][Stable] Empty path segments (`//`) are invalid.
- [PKG-IMPORT-003][Stable] Default alias is last normalized segment; explicit alias required if invalid identifier.
- [PKG-IMPORT-004][Stable] `import "path" as _ { ... }` is invalid.
- [PKG-IMPORT-005][Stable] Wildcard named import `*` is invalid.
- [PKG-IMPORT-006][Stable] Import cycles are invalid.
- [PKG-IMPORT-007][Stable] `import "platform"` is built-in package import.
- [PKG-IMPORT-008][Stable] Built-in `platform` currently exports `exit(status i32)`.
- [PKG-IMPORT-009][Provisional] Feature imports `slang/feature/<name>` / `feature/<name>` are pseudo-imports; unknown names warn in `Reference-slc`.
- [PKG-IMPORT-010][Stable] Primary resolution base is loader root:
  - directory package mode: parent directory of entry package directory
  - single-file package mode: directory containing the entry `.sl` file
- [PKG-IMPORT-011][Stable] For recognized library import paths (`builtin`, `reflect`, `compiler`, `mem`, `platform`, `std/*`, `platform/*`), resolver order is:
  1. try `<loader_root>/<importPath>` first
  2. if that path is not an existing directory, search `<ancestor>/lib/<importPath>` from importing package directory upward to filesystem root and select the nearest match
  3. if still unresolved, search `<ancestor>/lib/<importPath>` from `dirname(executable_path)` upward to filesystem root and select the nearest match
- [PKG-IMPORT-012][Stable] [PKG-IMPORT-011] is deterministic; there is no additional tie-break stage.

### 11.3 Exports and API closure
- [PKG-PUB-001][Stable] `pub` exports top-level declarations.
- [PKG-PUB-002][Stable] Duplicate exported symbol (same kind+name) is invalid.
- [PKG-PUB-003][Stable] Exported function declarations MUST have exactly one body definition in package.
- [PKG-PUB-004][Stable] Public API types may reference builtins, exported local types, and imported exported types.
- [PKG-PUB-005][Stable] Public API MUST NOT expose private local types.

## 12. Entrypoint and Program Rules

- [MAIN-SIG-001][Stable] Source-language main declaration is `fn main()` (no params, no return type, no explicit `context` clause).
- [MAIN-SIG-002][Stable] Inside `main`, contextual operations use an implicit root context value.
- [MAIN-SIG-003][Provisional] `Reference-slc` executable entry enforces `fn main()` at compile/run boundary.

## 13. Diagnostics Contract

- [DIAG-CLASS-001][Stable] Conformance requires rejection/acceptance behavior and error class, not exact diagnostic text.
- [DIAG-CLASS-002][Stable] Required error classes include:
  - `ParseError`
  - `NameResolutionError`
  - `TypeError`
  - `ContextError`
  - `ImportResolutionError`
  - `EntrypointError`

## 14. Reference-slc Profile (Implementation-Defined)

This section is non-core and documents current reference behavior.

- [REF-IMPL-001][Provisional] `switch` lowers to `if/else` chains.
- [REF-IMPL-002][Provisional] Optional unwrap lowers to runtime null-trap helper.
- [REF-IMPL-003][Provisional] `new` lowering uses allocator helpers and may inject implicit unwrap depending on destination optionality.
- [REF-IMPL-004][Provisional] Non-constant index/slice bounds checks are analyzed statically; runtime checks are not universally emitted.
- [REF-IMPL-005][Provisional] Implicit main context type resolution order:
  1. named `builtin__Context`
  2. named type matching `builtin*__Context`
  3. named `Context`
  4. fallback synthesized fields `mem` and `log`
- [REF-IMPL-006][Provisional] Optional feature imports are recognized but optional syntax is not hard-gated on those imports.
- [REF-IMPL-007][Provisional] Expression-switch lowering evaluates subject once and compares against case labels using a cached temporary, consistent with [STMT-SWITCH-007].
- [REF-IMPL-008][Provisional] `assert(cond, fmt, args...)` currently ignores formatting arguments in panic payload construction.
- [REF-IMPL-009][Provisional] `const` initializers are eagerly const-evaluated and rejected when non-const-evaluable (top-level and local).
- [REF-IMPL-010][Provisional] Current const-eval supports:
  - const-evaluable function calls with local declarations, `if`, `for`, `switch`, `assert`, `defer`, `break`, `continue`
  - `sizeof(Type)` and `sizeof(expr)` (including identifiers resolved from const-eval local/param bindings)
  - casts among numeric/bool forms, `string -> bool`, and `null -> rawptr` / `null -> ?T`
- [REF-IMPL-011][Provisional] Cast forms outside [REF-IMPL-010] may typecheck by [EXPR-CAST-002] but still be non-const-evaluable in `Reference-slc`.

## 15. Evolution Policy

- [EVOL-POLICY-001][Stable] New language changes MUST be introduced by adding/updating rule IDs, not by implicit prose drift.
- [EVOL-POLICY-002][Stable] Rule status transitions are explicit: `Draft -> Provisional -> Stable`.
- [EVOL-POLICY-003][Stable] Breaking changes to `Stable` rules require a migration note and conformance update.

## 16. Draft (Non-Normative)

- [DRAFT-SLP17][Draft] Platform-context composition (`platform/<target>.Context`) and capability expansion.
- [DRAFT-SLP18][Draft] Reflection and `typeof` metatype APIs.
