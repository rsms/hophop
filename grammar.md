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

LineComment    = "//" { ? any char except newline ? } ;

Whitespace     = " " | "\t" | "\r" | "\n" ;
```

### Keywords

```ebnf
Keyword =
    "package" | "import" | "pub" |
    "struct" | "union" | "enum" |
    "fn" | "var" | "const" |
    "if" | "else" |
    "for" |
    "switch" | "case" | "default" |
    "break" | "continue" | "return" |
    "defer" | "assert" |
    "as" |
    "true" | "false" ;
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
SourceFile      = PackageClause ";" { ImportDecl ";" } { TopLevelDecl [ ";" ] } EOF ;

PackageClause   = "package" Identifier ;

ImportDecl      = "import" [ Identifier ] StringLit ;

TopLevelDecl    = PubBlock
                | StructDecl
                | UnionDecl
                | EnumDecl
                | FunDecl
                | FunDef
                | ConstDecl ;

PubBlock        = "pub" "{" { PubDecl [ ";" ] } "}" ;
PubDecl         = StructDecl
                | UnionDecl
                | EnumDecl
                | FunDecl
                | ConstDecl ;

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

Type            = PointerType | ArrayType | TypeName ;
PointerType     = "*" Type ;
ArrayType       = "[" IntLit "]" Type ;
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

PostfixExpr         = PrimaryExpr { CallSuffix | IndexSuffix | SelectorSuffix | CastSuffix } ;
CallSuffix          = "(" [ ArgList ] ")" ;
ArgList             = Expr { "," Expr } ;
IndexSuffix         = "[" Expr "]" ;
SelectorSuffix      = "." Identifier ;
CastSuffix          = "as" Type ;

PrimaryExpr         = Identifier
                    | IntLit
                    | FloatLit
                    | BoolLit
                    | StringLit
                    | CompoundLit
                    | "(" Expr ")" ;

CompoundLit         = Type "{" [ FieldInitList [ "," ] ] "}" ;
FieldInitList       = FieldInit { "," FieldInit } ;
FieldInit           = Identifier "=" Expr ;
```

## 5. Notes

- `if`, `for`, `switch`, `case`, and `default` bodies are blocks in syntax.
- `pub { ... }` contains declarations only (no function bodies).
- Import paths are string literals.
