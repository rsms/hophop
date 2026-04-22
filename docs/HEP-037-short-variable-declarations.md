# HEP-37: Short variable declarations

Status: Provisional

## Summary

Add local statement syntax `name := expr` and `name1, name2 := expr1, expr2`.

For each non-blank left-hand name, `:=` assigns to an existing visible local when one exists, otherwise it declares a new mutable local whose type is inferred like `var name = expr`. This differs from Go: an all-existing short assignment is valid and behaves like assignment.

## Syntax

```ebnf
ShortAssignStmt = DeclNameList ":=" ExprList .
ForInit         = VarDeclStmt | ShortAssignStmt | Expr .
```

`ShortAssignStmt` is local statement syntax only. It is accepted in blocks and in `for` init clauses. It is not an expression and is not valid at top level.

The left-hand side is limited to declaration names: identifiers and `_`. Arbitrary assignment targets such as fields, indexes, or dereferences are not accepted.

## Typing

The right-hand side arity matches grouped `var` declarations and multi-assignment: either the number of RHS expressions equals the number of LHS names, or a single tuple-typed RHS is decomposed positionally.

Before adding new locals, all non-blank LHS names are classified in the current visible local scope. Existing locals must be mutable and assignable from the corresponding RHS type. New locals infer and concretize their type like `var name = expr`; inference from `null`, `void`, or invalid by-value varsize types is rejected.

`_` evaluates and discards the corresponding RHS value. It never declares a local and never resolves an existing local.

Duplicate non-blank LHS names in one short assignment are invalid.

## Lowering

All RHS expressions are evaluated first into temporaries. Then LHS actions are applied left-to-right: existing locals are stored, new locals are declared and initialized, and blanks are ignored after evaluation.
