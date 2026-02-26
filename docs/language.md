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
- [LEX-COMMENT-002][Stable] Block comments are not part of the language.

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
  `import pub struct union enum fn var const type if else for switch case default break continue return defer assert sizeof true false as null new context with`.
- [LEX-KW-002][Stable] `mut` is reserved legacy syntax and MUST be rejected in type positions.

### 2.4 Literals
- [LEX-LIT-001][Stable] Integer literals: decimal or hex (`0x` / `0X`).
- [LEX-LIT-002][Stable] Float literals: decimal with fraction and/or exponent.
- [LEX-LIT-003][Stable] String literals are either interpreted (`"..."`) or raw (`` `...` ``).
- [LEX-LIT-004][Stable] Boolean literals: `true`, `false`.
- [LEX-LIT-005][Stable] Null literal: `null`.
- [LEX-LIT-006][Provisional] Interpreted-string escapes follow Go-style forms: `\a`, `\b`, `\f`, `\n`, `\r`, `\t`, `\v`, `\\`, `\"`, `\'`, octal `\NNN`, hex `\xNN`, Unicode `\uNNNN`, `\UNNNNNNNN`.
- [LEX-LIT-007][Provisional] Both interpreted and raw string literals may span multiple source lines. Source line endings are normalized to `\n` in decoded string bytes.
- [LEX-LIT-008][Provisional] In interpreted strings, `\` immediately followed by a line break elides that line break. In raw strings, only ``\` `` is treated specially (it encodes a literal backtick).
- [LEX-LIT-009][Provisional] String literals MUST decode to valid UTF-8 byte sequences.

### 2.5 Semicolon insertion
- [LEX-SEMI-001][Stable] The formal syntax uses semicolons `";"` as terminators in a number of productions. SL programs may omit most of these semicolons using the following two rules:
  1. A semicolon is inserted at newline when the preceding token can end a statement.
  2. A semicolon is inserted at EOF when the final token can end a statement.
- [LEX-SEMI-002][Stable] Statement-ending tokens are exactly:
  - `IDENT`, `INT`, `FLOAT`, `STRING`, `TRUE`, `FALSE`, `NULL`
  - `BREAK`, `CONTINUE`, `RETURN`
  - `RPAREN`, `RBRACK`, `RBRACE`
  - `NOT` (postfix unwrap `!` at line end)
  - `CONTEXT` (when tokenized as identifier-like expression operand)
- [LEX-SEMI-003][Stable] In section 3 productions, semicolons are modeled as separators between adjacent declarations/statements in lists.

## 3. Concrete Syntax

### 3.1 File structure
- [SYN-FILE-001][Stable] `SourceFile` consists of zero or more imports followed by zero or more top-level declarations.
- [SYN-FILE-002][Stable] Imports MUST precede top-level declarations.
- [SYN-FILE-003][Stable] Section 3 grammar is parser grammar over the token stream after lexical processing and semicolon insertion ([LEX-SEMI-001], [LEX-SEMI-002]).

### 3.2 EBNF

```ebnf
SourceFile      = { ImportDecl ";" } { TopDecl ";" } .
StringLit       = /* lexical string literal; see [LEX-LIT-006] through [LEX-LIT-009] */ .

ImportDecl      = "import" StringLit [ ImportAlias ] [ ImportSymbols ] .
ImportAlias     = "as" ( Ident | "_" ) .
ImportSymbols   = "{" [ ImportSymbolList ] "}" .
ImportSymbolList = ImportSymbol { ImportSep ImportSymbol } [ ImportSep ] .
ImportSymbol    = Ident [ "as" Ident ] .
ImportSep       = "," | ";" .

TopDecl         = [ "pub" ] ( StructDecl | UnionDecl | EnumDecl | TypeAliasDecl | FnDeclOrDef | FnGroupDecl | TopConstDecl ) .

StructDecl      = "struct" Ident "{" [ StructFieldDeclList ] "}" .
UnionDecl       = "union" Ident "{" [ FieldDeclList ] "}" .
EnumDecl        = "enum" Ident Type "{" [ EnumItemList ] "}" .
TypeAliasDecl   = "type" Ident Type .
FieldSep        = "," | ";" .

StructFieldDeclList = StructFieldDecl { FieldSep StructFieldDecl } [ FieldSep ] .
FieldDeclList   = FieldDecl { FieldSep FieldDecl } [ FieldSep ] .
EnumItemList    = EnumItem { FieldSep EnumItem } [ FieldSep ] .
StructFieldDecl = ( FieldDecl | EmbeddedFieldDecl ) [ FieldDefault ] .
FieldDecl       = Ident { "," Ident } Type .
EmbeddedFieldDecl = TypeName .
FieldDefault    = "=" Expr .
EnumItem        = Ident [ "=" Expr ] .

FnDeclOrDef     = "fn" FnName "(" [ ParamList ] ")" [ Type ] [ ContextClause ] [ Block ] .
FnGroupDecl     = "fn" FnName "{" GroupMemberList "}" .
FnName          = Ident | "sizeof" .
GroupMemberList = GroupMember { "," GroupMember } .
GroupMember     = Ident | Ident "." Ident { "." Ident } .
ParamList       = ParamGroup { "," ParamGroup } .
ParamGroup      = ( Ident | "_" ) { "," ( Ident | "_" ) } Type .
ContextClause   = "context" Type .

TopConstDecl    = "const" Ident ( [ Type ] "=" Expr ) .
LocalConstDecl  = "const" ( Ident | "_" ) ( [ Type ] "=" Expr ) .

Type            = OptionalType | PtrType | RefType | SliceType | ArrayType | VarArrayType
                | FnType | AnonStructType | AnonUnionType | TypeName .
OptionalType    = "?" Type .
PtrType         = "*" Type .
RefType         = "&" Type .
SliceType       = "[" Type "]" .
ArrayType       = "[" Type IntLit "]" .
VarArrayType    = "[" Type "." Ident "]" .
FnType          = "fn" "(" [ FnTypeParamList ] ")" [ Type ] .
FnTypeParamList = FnTypeParam { "," FnTypeParam } .
FnTypeParam     = Type | ( Ident { "," Ident } Type ) .
AnonStructType  = [ "struct" ] "{" [ FieldDeclList ] "}" .
AnonUnionType   = "union" "{" [ FieldDeclList ] "}" .
TypeName        = Ident { "." Ident } .

Block           = "{" [ StmtList ] "}" .
StmtList        = Stmt { ";" Stmt } [ ";" ] .
Stmt            = Block | VarDeclStmt | LocalConstDecl | IfStmt | ForStmt | SwitchStmt
                | ReturnStmt | BreakStmt | ContinueStmt | DeferStmt | AssertStmt | ExprStmt .

VarDeclStmt     = "var" ( Ident | "_" ) ( Type [ "=" Expr ] | "=" Expr ) .
IfStmt          = "if" Expr Block [ "else" ( IfStmt | Block ) ] .
ForStmt         = "for" ( Block | Expr Block | ForClause Block ) .
ForClause       = [ ForInit ] ";" [ Expr ] ";" [ Expr ] .
ForInit         = VarDeclStmt | Expr .
SwitchStmt      = "switch" [ Expr ] "{" { CaseClause } [ DefaultClause ] "}" .
CaseClause      = "case" Expr { "," Expr } Block .
DefaultClause   = "default" Block .
ReturnStmt      = "return" [ Expr ] .
BreakStmt       = "break" .
ContinueStmt    = "continue" .
DeferStmt       = "defer" ( Block | Stmt ) .
AssertStmt      = "assert" Expr [ "," Expr { "," Expr } ] .
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
PostfixSuffix   = CallWithContextSuffix | IndexSuffix | SelectorSuffix | CastSuffix | UnwrapSuffix .
CallWithContextSuffix = CallSuffix [ WithContextClause ] .
CallSuffix      = "(" [ ExprList ] ")" .
ExprList        = Expr { "," Expr } .
WithContextClause = "with" ( "context" | ContextOverlay ) .
ContextOverlay  = "{" [ ContextBindList ] "}" .
ContextBindList = ContextBind { "," ContextBind } [ "," ] .
ContextBind     = Ident [ "=" Expr ] .
IndexSuffix     = "[" Expr "]" | "[" [ Expr ] ":" [ Expr ] "]" .
SelectorSuffix  = "." Ident .
CastSuffix      = "as" Type .
UnwrapSuffix    = "!" .

PrimaryExpr     = Ident | "context" | IntLit | FloatLit | StringLit | BoolLit | "null"
                | CompoundLit | NewExpr | "sizeof" "(" ( Type | Expr ) ")" | "(" Expr ")" .
NewExpr         = "new" ( "[" Type Expr "]" | Type [ "{" [ FieldInitList ] "}" ] ) [ "with" Expr ] .
CompoundLit     = [ TypeName ] "{" [ FieldInitList ] "}" .
FieldInitList   = FieldInit { "," FieldInit } [ "," ] .
FieldInit       = Ident { "." Ident } "=" Expr .
```

### 3.3 Parsing disambiguation rules
- [SYN-DISAMBIG-001][Stable] In function-type parameter lists, parser disambiguation for each comma-delimited segment is:
  1. try `Ident {"," Ident} Type` as a named-parameter group
  2. if that parse fails, backtrack and parse the segment as `Type`
- [SYN-DISAMBIG-002][Stable] Function-type parameter segments are parsed left-to-right with the above backtracking rule.
- [SYN-DISAMBIG-003][Stable] `context` is always lexed as keyword token `CONTEXT`; when accepted as a primary expression it is represented as identifier-like operand, never as a bindable identifier declaration name.
- [SYN-DISAMBIG-004][Stable] In aggregate field/item lists (`struct`, `union`, `enum`, anonymous aggregate types), separators are required between consecutive entries; only a final trailing separator is optional.
- [SYN-DISAMBIG-005][Stable] In statement context (`Stmt`), leading `{` is parsed unconditionally as `Block`.
- [SYN-DISAMBIG-006][Stable] `ExprStmt` in statement context MUST NOT consume unparenthesized brace-leading compound-literal forms (`{ ... }`). If a compound-literal expression statement is intended, it MUST be parenthesized (`({ ... })`).

Canonical examples:

```sl
fn f() {
    { x = 1 }      // block with one statement: assignment to x
}
```

```sl
fn f() {
    ({ x = 1 })    // expression statement: compound literal value discarded
}
```

```sl
fn f() {
    defer { x = 1 }    // deferred block
}
```

## 4. Declarations, Scope, and Binding

- [DECL-TOP-001][Stable] Top-level declaration kinds: `fn` declarations/definitions (including function-group declarations `fn Name{...}`), `struct`, `union`, `enum`, `type`, `const`.
- [DECL-TOP-002][Stable] Top-level `var` is invalid.
- [DECL-TOP-003][Stable] `const` MUST have an initializer.
- [DECL-TOP-004][Stable] `pub` applies to a single following top-level declaration.
- [DECL-HOLE-001][Stable] `_` MUST NOT name top-level symbols, struct/union fields, enum items, or type aliases.
- [DECL-HOLE-002][Stable] Local discard declarations `var _ = expr` and `const _ = expr` are valid statement forms.
- [DECL-SCOPE-001][Stable] Scope is lexical by block; nearest declaration wins.
- [DECL-SCOPE-002][Stable] Function/type declarations are collected before body checking (declaration-order independent).

### 4.1 Function declarations and groups
- [DECL-FN-001][Stable] Multiple declarations for the same function signature are allowed; at most one definition body is allowed.
- [DECL-FN-002][Stable] For a fixed function signature, `context` clauses MUST match exactly.
- [DECL-FN-003][Stable] `context` is not an overload dimension.
- [DECL-FN-004][Stable] `sizeof` is accepted as a declaration name for builtin-signature compatibility, but `sizeof(...)` expression syntax is always the builtin form and is not shadowable by user functions.
- [DECL-FNGROUP-001][Stable] `fn Group{...}` defines explicit overload set membership.
- [DECL-FNGROUP-001A][Stable] Function-group declarations follow normal top-level declaration separators: separating semicolon token may be explicit `;` or inserted automatically after closing `}` at newline/EOF by [LEX-SEMI-001].
- [DECL-FNGROUP-002][Stable] Group members MUST refer to existing functions.
- [DECL-FNGROUP-003][Stable] Group members MUST be unique.
- [DECL-FNGROUP-004][Stable] Group name MUST NOT collide with an existing symbol.

### 4.2 Struct composition and enum member scope
- [DECL-EMBED-001][Stable] `struct` may embed one base field (type-name-only) as first field.
- [DECL-EMBED-002][Stable] Embedded base MUST be a named struct type.
- [DECL-EMBED-003][Stable] Embedded cycles are invalid.
- [DECL-EMBED-004][Stable] Field lookup order is direct fields first, then embedded chain recursively.
- [DECL-ENUM-001][Stable] Enum item names are scoped to enum type; items are not package-global bindings.
- [DECL-ENUM-002][Stable] Enum values are selected as `Enum.Item` or `pkg.Enum.Item`.
- [DECL-ENUM-003][Stable] Enum base type in `enum Name BaseType { ... }` MUST be an integer type.
- [DECL-ENUM-004][Stable] There are no implicit conversions between enum types and integer types; explicit `as` casts are required.

### 4.3 Namespace model
- [DECL-NS-001][Stable] Type names and value names are distinct lookup spaces.
- [DECL-NS-002][Stable] Function overload sets live in value-name space.
- [DECL-NS-003][Stable] Import bindings (`as` aliases and named imports) MUST NOT collide with any top-level declaration name in the importing package.
- [DECL-NS-004][Stable] Duplicate top-level names are invalid within a namespace, except function overloading and declaration/definition pairing allowed by [DECL-FN-001].

## 5. Type System

### 5.1 Built-in and constructed types
- [TYPE-BUILTIN-001][Stable] Built-ins include `void`, `bool`, and `str`.
- [TYPE-BUILTIN-002][Stable] Source-level numeric type names are:
  - unsigned integers: `u8`, `u16`, `u32`, `u64`, `uint`
  - signed integers: `i8`, `i16`, `i32`, `i64`, `int`
  - floating point: `f32`, `f64`
- [TYPE-BUILTIN-003][Stable] `int` and `uint` are pointer-sized signed/unsigned integers for the target.
- [TYPE-BUILTIN-004][Stable] `Allocator` is a source-level type provided by core library declarations (for example `core.Allocator` and implicit core imports), not a language builtin type.
- [TYPE-CONSTR-001][Stable] Constructed types: pointers `*T`, references `&T`, arrays `[T N]`, slices `[T]`, dependent arrays `[T .n]`, optionals `?T`, function types, anonymous aggregates.
- [TYPE-CONSTR-002][Stable] `[T]` is unsized and MUST NOT be used by value.
- [TYPE-CONSTR-003][Stable] Function type return type defaults to `void` when omitted.
- [TYPE-CONSTR-004][Stable] Function type explicit return type MUST NOT be `void`.
- [TYPE-CONSTR-005][Stable] Variable-size-by-value types are invalid in local/param/return/function-type positions.

### 5.2 Mutability model
- [TYPE-MUT-001][Stable] `*T` is writable reference-like access to `T`.
- [TYPE-MUT-002][Stable] `&T` is read-only reference-like access to `T`.
- [TYPE-MUT-003][Stable] Slice mutability follows wrapper mutability:
  - `*[T]` / `*[T N]` are writable slice views
  - `&[T]` / `&[T N]` are read-only slice views
- [TYPE-MUT-004][Stable] There is no source syntax for `mut` references or slices.

### 5.3 Optional types
- [TYPE-OPT-001][Stable] `?T` currently accepts only pointer/reference families (`*...` / `&...`, including array/slice forms).
- [TYPE-OPT-002][Stable] `null` is assignable only to optional types.
- [TYPE-OPT-003][Stable] Implicit lift `T -> ?T` is allowed.
- [TYPE-OPT-004][Stable] `?T -> T` is not implicit; unwrap or narrowing is required.
- [TYPE-OPT-005][Stable] Flow narrowing for optionals is restricted to direct null checks on local identifiers (including parameters):
  - `x == null`, `x != null`, `null == x`, `null != x`
  - `if x == null { ... } else { ... }` narrows `x` to `null` in `then`, `T` in `else`
  - `if x != null { ... } else { ... }` narrows `x` to `T` in `then`, `null` in `else`
  - continuation narrowing after `if` without `else` applies only when the `then` branch terminates (`return`, `break`, or `continue`)
- [TYPE-OPT-006][Provisional] Feature imports `slang/feature/optional` and `feature/optional` are recognized but not required to enable `?` in `Reference-slc`.

### 5.4 Assignability and coercion
- [TYPE-ASSIGN-001][Stable] Assignment requires exact type match except explicit implicit-conversion rules.
- [TYPE-ASSIGN-002][Stable] Untyped literals convert to compatible concrete numeric types.
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

### 5.5 Inference and zero values
- [TYPE-INFER-001][Stable] `var x = expr` and `const x = expr` infer from `expr` after concretization.
- [TYPE-INFER-002][Stable] `untyped_int` defaults to `int`; `untyped_float` defaults to `f64`.
- [TYPE-INFER-003][Stable] Inference from `null` or `void` expressions is invalid.
- [TYPE-ZERO-001][Stable] `var x T` zero-initializes `x`.

## 6. Expressions and Operators

- [EXPR-OP-001][Stable] Operator precedence is: postfix, unary, multiplicative, additive, shift, relational, equality, bit-and, bit-xor, bit-or, logical-and, logical-or, assignment.
- [EXPR-UNARY-001][Stable] Unary `+`/`-` require numeric operands; unary `!` requires bool.
- [EXPR-UNARY-002][Stable] Unary `*` dereferences pointer/reference; unary `&` forms read-only references.
- [EXPR-ASSIGN-001][Stable] Assignment LHS MUST be assignable (identifier/index/non-dependent field/dereference of writable location).
- [EXPR-ASSIGN-002][Stable] Compound assignment requires assignable LHS and numeric LHS type.
- [EXPR-CMP-001][Stable] Equality/ordering require coercion to a common comparable/ordered type, except optional-null equality special-case.
- [EXPR-CAST-001][Stable] `as` is explicit cast syntax.
- [EXPR-CAST-002][Stable] A cast expression is well-typed iff source expression typing succeeds and target type resolution succeeds; no additional cast-compatibility gate is applied by Core static semantics.
- [EXPR-CAST-003][Provisional] Future profiles may add stricter cast-compatibility rules; `Reference-slc` currently follows [EXPR-CAST-002].
- [EXPR-UNWRAP-001][Stable] `x!` requires `x : ?T` and yields `T`.
- [EXPR-UNWRAP-002][Stable] Unwrapping `null` is a runtime trap (panic), never undefined behavior.
- [EXPR-ADD-001][Provisional] String `+` currently supports compile-time concatenation only for non-parenthesized literal chains (e.g. `"a" + "b" + "c"`). Other string `+` forms are invalid in `Reference-slc`.

### 6.1 Comparable and ordered types
- [EXPR-COMMON-001][Stable] Binary-operation common-type selection is:
  1. if `leftType == rightType`, use that type
  2. if left is untyped and right is typed and `left` is assignable to right, use right
  3. if right is untyped and left is typed and `right` is assignable to left, use left
  4. if pair is (`untyped_int`, `untyped_float`) or reversed, use `untyped_float`
  5. otherwise there is no common type
- [EXPR-COMMON-002][Stable] Equality/ordering comparisons that are not handled by optional-null special case or comparison hooks MUST use [EXPR-COMMON-001].
- [EXPR-CMPSET-001][Stable] Comparable types are:
  - `bool` and numeric types
  - string-like values (`str`, `*str`, `&str`)
  - pointers/references
  - arrays/slices whose element type is comparable
  - enum types
  - struct/union types whose fields are all comparable
  - optional types whose base type is comparable
- [EXPR-CMPSET-002][Stable] Ordered types are:
  - numeric types
  - string-like values (`str`, `*str`, `&str`)
  - pointers/references
  - arrays/slices whose element type is ordered
  - enum types
  - optional types whose base type is ordered
- [EXPR-CMPSEM-001][Stable] Operational comparison semantics are:
  - string-like: content-based bytewise equality/order
  - pointers/references (and pointer-vs-`null`): identity equality; ordering by pointer-address order
  - arrays/slices: bytewise sequence equality/order over `len * sizeof(element)`
  - `struct`/`union` equality: bytewise object-representation equality
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
  - `3`: untyped literal concretization conversion
  - `4`: optional lift `T -> ?T`
- [EXPR-SUGAR-009][Stable] Multi-step conversion-cost composition rules are:
  - alias-source peeling is recursive and additive (`cost(dst <- alias(src)) = cost(dst <- srcBase) + 1`)
  - optional-to-optional conversion recurses on base (`cost(?A <- ?B) = cost(A <- B)`)
  - all non-recursive assignable non-exact conversions default to cost `1` unless matched by a more specific cost rule above.

### 6.3 Compound literals
- [EXPR-COMPOUND-001][Stable] Compound literals are named-field only.
- [EXPR-COMPOUND-002][Stable] Field names may be dotted (`a.b.c = ...`).
- [EXPR-COMPOUND-003][Stable] Inferred `{ ... }` without explicit type requires expected aggregate type context or anonymous-struct inference.
- [EXPR-COMPOUND-004][Stable] Anonymous-struct inference from `{ field = expr, ... }` uses field names and concretized field value types.
- [EXPR-COMPOUND-005][Stable] Duplicate field initializer paths in the same literal are invalid.
- [EXPR-COMPOUND-006][Stable] Omitted fields are allowed:
  - struct fields without explicit initializer and without field-default evaluate to zero-value
  - struct fields with declaration default evaluate to their default expression unless suppressed by explicit direct-field initializer
  - union literals may initialize at most one field explicitly; with zero explicit fields the union is zero-initialized.
- [EXPR-COMPOUND-007][Stable] Explicit initializers take precedence over defaults for the same direct field.
- [EXPR-COMPOUND-008][Stable] Default-suppression matching is by direct field name; dotted subfield initializer paths do not suppress defaults of containing direct fields.

## 7. Statements and Control Flow

- [STMT-IF-001][Stable] `if` condition MUST be bool.
- [STMT-FOR-001][Stable] `for` forms: infinite block, condition form, and C-style `init; cond; post`.
- [STMT-FOR-002][Stable] `for` condition (if present) MUST be bool.
- [STMT-FOR-003][Stable] Variables declared in `for` initializer are scoped to the entire loop (condition, post, and body) and are not visible after the loop.
- [STMT-SWITCH-001][Stable] `switch` supports expression-switch and condition-switch.
- [STMT-SWITCH-002][Stable] At most one `default` clause.
- [STMT-SWITCH-003][Stable] No fallthrough.
- [STMT-SWITCH-004][Stable] Expression-switch case labels must be assignable to subject type; condition-switch labels must be bool.
- [STMT-SWITCH-005][Stable] Case labels are tested left-to-right and first matching case body executes.
- [STMT-SWITCH-006][Stable] Duplicate case labels are not required to be diagnosed statically.
- [STMT-SWITCH-007][Stable] Expression-switch semantics are defined as if subject expression is evaluated once before case-label matching.
- [STMT-CTRL-001][Stable] `break` valid inside `for`/`switch`; `continue` only inside `for`.
- [STMT-RETURN-001][Stable] `return expr` required iff function return type is non-void.
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
- [CTX-CALL-001][Stable] Calls without `with` forward effective current context automatically.
- [CTX-CALL-002][Stable] `with context` is explicit pass-through and equivalent to omission.
- [CTX-CALL-003][Stable] `with { ... }` creates call-local context overlay.
- [CTX-CALL-004][Stable] Overlay bind shorthand `name` means `name = context.name`.
- [CTX-CALL-005][Stable] Overlay duplicate field names are invalid.
- [CTX-CALL-006][Stable] Callee context requirements are structural-by-field: each required field name/type must be provided by effective context.
- [CTX-CALL-007][Stable] Overlay bindings take precedence over forwarded ambient fields with the same name.

## 9. Built-in Forms and Functions

### 9.1 `len(x)`
- [BI-LEN-001][Stable] Valid argument families: `str`, arrays, slices, pointers/references to arrays/slices.
- [BI-LEN-002][Stable] Return type is `u32`.
- [BI-LEN-003][Stable] Selector-call sugar `x.len()` is equivalent when no field `len` shadows it.

### 9.2 `cstr(s)`
- [BI-CSTR-001][Stable] Argument MUST be `str`-assignable.
- [BI-CSTR-002][Stable] Return type is `&u8`.
- [BI-CSTR-003][Stable] Selector-call sugar `x.cstr()` is supported.
- [BI-CSTR-004][Stable] Returned pointer references the source string byte storage and remains valid while that storage remains valid.
- [BI-CSTR-005][Stable] For non-null string values the returned byte sequence is NUL-terminated.

### 9.3 `new`
- [BI-NEW-001][Stable] Forms:
  - `new T`
  - `new T{...}`
  - `new [T n]`
  - each with optional `with allocExpr`
- [BI-NEW-002][Stable] Without explicit allocator, effective context MUST provide `mem` compatible with `*Allocator`.
- [BI-NEW-003][Stable] `n` in `new [T n]` MUST be integer-typed; constant negative values are invalid.
- [BI-NEW-004][Stable] Variable-size element types require initializer in non-count form.
- [BI-NEW-005][Stable] Static result typing:
  - `new T` -> `*T`
  - `new [T n]` -> `*[T N]` when `n` is constant positive `N`, else `*[T]`
- [BI-NEW-005A][Stable] `new [T 0]` has type `*[T]` (runtime-length slice pointer form).
- [BI-NEW-006][Provisional] In `Reference-slc`, codegen may insert implicit null-trap unwrap when coercing `new` into non-optional pointer destinations.

### 9.4 `concat(a, b)` and `free(...)`
- [BI-CONCAT-001][Stable] `concat(a, b)` requires both args `str`-assignable and context `mem`; returns `*str`.
- [BI-FREE-001][Stable] `free(value)` uses context allocator; `free(alloc, value)` uses explicit allocator.
- [BI-FREE-002][Stable] Method sugar `alloc.free(value)` is supported.

### 9.5 `panic(msg)`
- [BI-PANIC-001][Stable] Argument MUST be `str`-assignable.
- [BI-PANIC-002][Stable] Return type is `void`.

### 9.6 `sizeof`
- [BI-SIZEOF-001][Stable] `sizeof(Type)` and `sizeof(expr)` forms are supported.
- [BI-SIZEOF-002][Stable] Result type is `uint`.
- [BI-SIZEOF-003][Stable] Unsized and variable-size-by-value type operands are invalid in `sizeof(Type)`.

### 9.7 `print(msg)`
- [BI-PRINT-001][Stable] Argument MUST be `str`-assignable.
- [BI-PRINT-002][Stable] Effective context MUST provide field `log`.
- [BI-PRINT-003][Stable] Core type checking imposes no additional static shape/type requirement on `log` beyond field presence.
- [BI-PRINT-004][Provisional] `Reference-slc` currently validates `log` field presence at typecheck time and may rely on backend coercion at codegen time for concrete logger compatibility.

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
- [PKG-IMPORT-011][Stable] For recognized library import paths (`core`, `mem`, `platform`, `std/*`, `platform/*`), resolver order is:
  1. try `<loader_root>/<importPath>` first
  2. if that path is not an existing directory, search `<ancestor>/lib/<importPath>` from importing package directory upward to filesystem root
  3. select the first match encountered in that upward walk (nearest ancestor)
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
  1. named `core__Context`
  2. named type matching `core*__Context`
  3. named `Context`
  4. fallback synthesized fields `mem` and `log`
- [REF-IMPL-006][Provisional] Optional feature imports are recognized but optional syntax is not hard-gated on those imports.
- [REF-IMPL-007][Provisional] Current codegen lowering may re-evaluate expression-switch subject per case-label comparison; this is a known divergence from [STMT-SWITCH-007].
- [REF-IMPL-007A][Provisional] `Reference-slc` compatibility does not require reproducing [REF-IMPL-007]; both single-evaluation ([STMT-SWITCH-007]) and re-evaluation behavior are accepted.
- [REF-IMPL-008][Provisional] `assert(cond, fmt, args...)` currently ignores formatting arguments in panic payload construction.

## 15. Evolution Policy

- [EVOL-POLICY-001][Stable] New language changes MUST be introduced by adding/updating rule IDs, not by implicit prose drift.
- [EVOL-POLICY-002][Stable] Rule status transitions are explicit: `Draft -> Provisional -> Stable`.
- [EVOL-POLICY-003][Stable] Breaking changes to `Stable` rules require a migration note and conformance update.

## 16. Draft (Non-Normative)

- [DRAFT-SLP17][Draft] Platform-context composition (`platform/<target>.Context`) and capability expansion.
- [DRAFT-SLP18][Draft] Reflection and `typeof` metatype APIs.
