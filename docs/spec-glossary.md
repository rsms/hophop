# SL Spec Glossary

This glossary defines terms used normatively in [`docs/language.md`](./language.md).

- `Assignable`: Value of source type can be assigned to destination type under section 5 implicit-conversion rules.
- `Coercible to common type`: Two operand types can be converted to one shared type for binary operator typing.
- `Common type`: The result of [EXPR-COMMON-001] used for binary operator typing/comparison checks.
- `Comparison hook`: Special overload name `__equal` or `__order` used to define type-specific comparison behavior.
- `Lvalue`: Expression denoting storage location.
- `Assignable expression`: Expression valid on assignment LHS (identifier, valid index, non-dependent field selection, writable dereference).
- `Writable`: Location that permits mutation (`*T` dereference, writable slice-view element path, writable aggregate field path).
- `Optional`: Type `?T` representing an optionally-present `T` value.
- `Context keyword expression`: The reserved `context` primary-expression operand that resolves to the ambient context value when one exists.
- `Unwrap`: Postfix `!` operation turning `?T` into `T`, trapping on null at runtime.
- `Concretization`: Replacement of untyped literal type with default concrete type during inference.
- `Conversion cost`: Integer ranking used by overload resolution to compare implicit conversion quality.
- `Direct field initializer`: Compound-literal initializer whose path is a single field name (not dotted).
- `By-value variable-size type`: Type whose size depends on runtime field values and therefore cannot be used as local/param/return by value.
- `Object-representation equality`: Equality defined over raw bytes of a value representation.
- `Embedded base`: First struct field declared by type name only, enabling field promotion and upcast relations.
- `Struct-compatible`: A type that, after alias resolution, is a named struct type or an anonymous struct type.
- `Effective context`: Context visible to a call after applying implicit forwarding and optional `context { ... }` overlay.
- `Context overlay`: Call-site field bindings provided by `context { ... }`.
- `Context requirement`: Field set declared by callee `context Type`; each required field must be present and assignable in effective context.
- `Loader root`: Base directory used to resolve normalized import paths in package loading mode.
- `Function overload set`: All visible functions sharing the same call name.
- `Selector-call sugar`: Call-only rewrite candidate from `recv.f(args...)` to `f(recv, args...)` when field lookup does not resolve `f`.
- `Core conformance`: Implements all `Stable` rules in `docs/language.md`.
- `Reference-slc conformance`: Core conformance plus `Provisional` and section 14 reference-profile behaviors.
- `Error class`: Coarse diagnostic category (`ParseError`, `TypeError`, etc.) required for conformance; exact text is not.
