# SL Grammar (EBNF)

This file defines the grammar of SL in EBNF style.

Semantics, typing, and lowering rules live in `project.md`.

## 1. Lexical Grammar

```ebnf
Letter         = "A"…"Z" | "a"…"z" ;
Digit          = "0"…"9" ;
HexDigit       = Digit | "a"…"f" | "A"…"F" ;
BinDigit       = "0" | "1" ;

Identifier     = ( Letter | "_" ) { Letter | Digit | "_" } ;

DecIntLit      = Digit { Digit } ;
HexIntLit      = "0" ( "x" | "X" ) HexDigit { HexDigit } ;
BinIntLit      = "0" ( "b" | "B" ) BinDigit { BinDigit } ;   (* optional in v0 *)
IntLit         = HexIntLit | BinIntLit | DecIntLit ;

FloatLit       = Digit { Digit } "." { Digit } [ Exponent ]
               | Digit { Digit } Exponent ;
Exponent       = ( "e" | "E" ) [ "+" | "-" ] Digit { Digit } ;

BoolLit        = "true" | "false" ;

Escape         = "\\" ( "\\" | "\"" | "n" | "t" | "r" | "0" | "x" HexDigit HexDigit ) ;
StringChar     = ? any char except backslash, quote, newline ? | Escape ;
StringLit      = "\"" { StringChar } "\"" ;

QUESTION       = "?" ;

LineComment    = "//" { ? any char except newline ? } ;

Whitespace     = " " | "\t" | "\r" | "\n" ;
```

### Keywords

```ebnf
Keyword =
    "import" | "pub" |
    "struct" | "union" | "enum" |
    "fn" | "var" | "const" | "mut" |
    "if" | "else" |
    "for" |
    "switch" | "case" | "default" |
    "break" | "continue" | "return" |
    "defer" | "assert" |
    "sizeof" |
    "as" |
    "true" | "false" |
    "null" ;
```

### Semicolon Insertion

`";"` may be written explicitly. Otherwise a semicolon is inserted at end-of-line if the last
token before newline is one of:

- `Identifier`
- literal (`IntLit`, `FloatLit`, `StringLit`, `BoolLit`)
- `break`, `continue`, `return`
- `")"`, `"]"`, `"}"`

The grammar below is written using explicit semicolons.

## 2. Syntactic Grammar

```ebnf
SourceFile      = { ImportDecl ";" } { TopLevelDecl [ ";" ] } EOF ;

ImportDecl      = "import" [ Identifier ] StringLit ;

(* Feature imports: "slang/feature/<name>" opt into experimental language features.
   Known features: "optional" — enables T? optional type syntax (SLAST_TYPE_OPTIONAL).
   Unknown feature names produce a compiler warning but are otherwise ignored. *)
FeatureImport   = "import" StringLit ;   (* path starts with "slang/feature/" *)

TopLevelDecl    = [ "pub" ] (
                    StructDecl
                  | UnionDecl
                  | EnumDecl
                  | FunDecl
                  | FunDef
                  | ConstDecl
                  ) ;

StructDecl      = "struct" Identifier "{" { FieldDecl [ FieldSep ] } "}" ;
UnionDecl       = "union"  Identifier "{" { FieldDecl [ FieldSep ] } "}" ;
FieldDecl       = Identifier Type ;
FieldSep        = "," | ";" ;

EnumDecl        = "enum" Identifier Type "{" { EnumItem [ EnumSep ] } "}" ;
EnumItem        = Identifier [ "=" Expr ] ;
EnumSep         = "," | ";" ;

FunDecl         = "fn" Identifier Signature ";" ;
FunDef          = "fn" Identifier Signature Block ;

Signature       = "(" [ ParamList ] ")" [ Type ] ;
ParamList       = Param { "," Param } ;
Param           = Identifier Type ;

ConstDecl       = "const" Identifier Type "=" Expr ";" ;

PointerType     = "*" Type ;
RefType         = "&" NonSliceType ;
MutRefType      = "mut" "&" NonSliceType ;
SliceType       = "[" Type "]" ;
MutSliceType    = "mut" "[" Type "]" ;
ArrayType       = "[" Type IntLit "]" ;
VarArrayType    = "[" Type "." Identifier "]" ;
OptionalType    = "?" BaseType ;   (* requires import "slang/feature/optional" *)
BaseType        = PointerType | RefType | MutRefType | SliceType | MutSliceType
                | ArrayType | VarArrayType | TypeName ;
NonSliceType    = PointerType | RefType | MutRefType | ArrayType | VarArrayType | TypeName ;
Type            = OptionalType
                | PointerType
                | RefType
                | MutRefType
                | SliceType
                | MutSliceType
                | ArrayType
                | VarArrayType
                | TypeName ;
TypeName        = Identifier { "." Identifier } ;
```

## 3. Statements

```ebnf
Block           = "{" { Stmt [ ";" ] } "}" ;

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

VarDeclStmt     = "var" Identifier Type [ "=" Expr ] ";" ;

IfStmt          = "if" Expr Block [ "else" ( IfStmt | Block ) ] ;

ForStmt         = "for" (
                    Block
                  | Expr Block
                  | [ ForInit ] ";" [ Expr ] ";" [ Expr ] Block
                  ) ;
ForInit         = VarDeclFor | Expr ;
VarDeclFor      = "var" Identifier Type [ "=" Expr ] ;

SwitchStmt      = "switch" [ Expr ] "{" { CaseClause } [ DefaultClause ] "}" ;
CaseClause      = "case" ExprList Block ;
DefaultClause   = "default" Block ;
ExprList        = Expr { "," Expr } ;

ReturnStmt      = "return" [ Expr ] ";" ;
BreakStmt       = "break" ";" ;
ContinueStmt    = "continue" ";" ;

DeferStmt       = "defer" ( Block | Stmt ) ;

AssertStmt      = "assert" Expr [ "," Expr { "," Expr } ] ";" ;

ExprStmt        = Expr ";" ;
```

## 4. Expressions (Precedence and Associativity)

Highest precedence is at the bottom.

```ebnf
Expr                = AssignExpr ;

AssignExpr          = LogicalOrExpr
                    | LogicalOrExpr AssignOp AssignExpr ;    (* right-associative *)
AssignOp            = "=" | "+=" | "-=" | "*=" | "/=" | "%="
                    | "&=" | "|=" | "^=" | "<<=" | ">>=" ;

LogicalOrExpr       = LogicalAndExpr { "||" LogicalAndExpr } ;
LogicalAndExpr      = BitOrExpr { "&&" BitOrExpr } ;
BitOrExpr           = BitXorExpr { "|" BitXorExpr } ;
BitXorExpr          = BitAndExpr { "^" BitAndExpr } ;
BitAndExpr          = EqualityExpr { "&" EqualityExpr } ;
EqualityExpr        = RelExpr { ( "==" | "!=" ) RelExpr } ;
RelExpr             = ShiftExpr { ( "<" | ">" | "<=" | ">=" ) ShiftExpr } ;
ShiftExpr           = AddExpr { ( "<<" | ">>" ) AddExpr } ;
AddExpr             = MulExpr { ( "+" | "-" ) MulExpr } ;
MulExpr             = UnaryExpr { ( "*" | "/" | "%" ) UnaryExpr } ;

UnaryExpr           = ( "+" | "-" | "!" | "*" | "&" ) UnaryExpr
                    | PostfixExpr ;

PostfixExpr         = PrimaryExpr { CallSuffix | IndexSuffix | SelectorSuffix | CastSuffix | UnwrapSuffix } ;
CallSuffix          = "(" [ ArgList ] ")" ;
ArgList             = Expr { "," Expr } ;
IndexSuffix         = "[" Expr "]"
                    | "[" [ Expr ] ":" [ Expr ] "]" ;
SelectorSuffix      = "." Identifier ;
CastSuffix          = "as" Type ;
UnwrapSuffix        = "!" ;   (* requires operand of optional type ?T; result is T; panics if null *)

PrimaryExpr         = Identifier
                    | IntLit
                    | FloatLit
                    | BoolLit
                    | StringLit
                    | "null"
                    | SizeofExpr
                    | CompoundLit
                    | "(" Expr ")" ;

SizeofExpr       = "sizeof" "(" ( Type | Expr ) ")" ;

CompoundLit         = Type "{" [ FieldInitList [ "," ] ] "}" ;
FieldInitList       = FieldInit { "," FieldInit } ;
FieldInit           = Identifier "=" Expr ;
```

## 5. Notes

- `if`, `for`, `switch`, `case`, and `default` bodies are blocks in syntax.
- `pub` marks an exported top-level declaration.
- Import paths are string literals.
- `&[T]` and `mut&[T]` are not valid type forms; use `[T]` and `mut[T]`.
- In `Signature`, the optional `Type` is the return type. Omitting it means the function returns no value. Writing `void` explicitly is a type error; omit the return type instead.
- `?T` (OptionalType) is an experimental feature; it requires `import "slang/feature/optional"` at the top of the file. Without that import the `?` token in a type position is a syntax error. `?` is valid only on pointer/reference/slice base types (`?*T`, `?&T`, `?mut&T`, `?[T]`, `?mut[T]`); use on plain value types (e.g. `?i32`) is deferred.
- `null` is a keyword/literal assignable only to optional types (`?T`). Assigning `null` to a non-optional type is a type error.
- Postfix `!` (UnwrapSuffix) unwraps an optional value; the operand must have type `?T` and the result has type `T`. At runtime it panics (via `SL_TRAP()`) if the value is null.
- Imports of the form `import "slang/feature/<name>"` are feature flags, not real packages: they are never resolved on disk and produce a warning for unknown feature names.
