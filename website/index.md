---
title: hophop programming language
---

**hophop** is small programming language in the C/Go family.
The reference compiler is written in strict C11 and can run programs through an evaluator, generate WASM modules and generate freestanding C code of a hophop package.

[Source on GitHub](https://github.com/rsms/hophop)<br>
[Language specification](spec/) [[md]](spec/index.md)

## Example

```hop
fn main() {
    greetings := [
    	"Hej världen!"
        "Hello, world!"
        "¡Hola Mundo!"
        "Γειά σου Κόσμε!"
        "Привіт, світе!"
        "こんにちは世界！"
    ]
    for greeting in greetings {
        print(greeting)
    }
}
```

Run it directly:

```sh
$ hophop run hello.hop
hej världen
```

Compile to wasm:

```sh
$ hophop genpkg:wasm hello.hop hello.wasm
```

## Language guide

These sections are ordered from high-impact, small ideas to lower-level and more specialized features.

### Functions

Functions are declared with `fn`. A definition has a body, while a declaration can omit the body when another package or backend provides the implementation. Programs run from `fn main()` with no parameters and no return type.

Parameters use name-then-type order, and a single return type follows the parameter list. Calls use ordinary expression syntax.

```hop
fn add(a, b i32) i32 {
	return a + b
}

fn main() {
	print("sum")
	add(1, 2)
}
```

HopHop uses [uniform function call syntax](https://en.wikipedia.org/wiki/Uniform_function_call_syntax) to support `expr.function()` calls without special syntax or a distinction between functions and methods.

```hop
struct Vec2 {
    x, y f64
}

fn mul(v Vec2, exp f64) Vec2 {
    return { x: v.x*exp, y: v.y*v.y }
}

fn main() {
    var v = Vec2{ x: 3.0, y: 4.0 }
    assert mul(v, exp: 4.3) == v.mul(4.3)
}
```

### Comments

HopHop accepts `//` line comments and `/* ... */` block comments. Block comments may nest, which makes it possible to comment out code that already contains block comments.

Most semicolons are inserted from newlines, so ordinary code is written one statement per line.

```hop
fn main() {
	// line comment
	/* outer /* nested */ outer */
	print("comments")
}
```

### Literals

Literals cover numbers, strings, runes, booleans and `null`. Strings can be interpreted with escapes or raw with backticks.

Rune literals use single quotes and represent one Unicode scalar value. `null` is only assignable where the type explicitly accepts it, such as optionals and `rawptr`.

```hop
n      := 42
hex    := 0xff
pi     := 3.14
msg    := "hello\n"
raw    := `hello\n`
letter := 'å'
ok     := true
array  := [1, 2, 3]
```

### Variables

`var` creates mutable local storage. `const` creates a compile-time value and must have an initializer.

The type can be written explicitly or inferred from the initializer. `var x T` zero-initializes most types, while direct pointer and reference locals must be assigned before use.

```hop
fn main() {
	const limit = 10
	var count     = 0
	var total i32 = 0
	var ready bool
}
```

`:=` is local short-assignment syntax. It assigns to an existing local when one exists, otherwise it declares a new mutable local inferred from the right-hand side.

Regular assignment uses `=`, compound assignment uses operators such as `+=`, and multi-assignment evaluates right-hand sides before storing left-to-right.

```hop
fn main() {
	x := 1
	x += 2
	y, z := 3, 4
	y, z = z, y
}
```

### Lexical scope

Names are block scoped, and the nearest declaration wins. The `_` name is a discard hole: it accepts a value without creating or updating a binding.

Top-level declarations are collected before function bodies are checked, so functions can call declarations that appear later in the file.

```hop
fn main() {
	var x = 1
	{
		var x = 2
		var _, y = x, 3
		assert y == 3
	}
	assert x == 1
}
```

### Types

Built-in types include `bool`, fixed-width integers, pointer-sized `int` and `uint`, `f32`, `f64`, `rawptr` and `type`. Text uses `str`, and `rune` is a Unicode codepoint.

Constant numeric expressions use `const_int` and `const_float` until they are assigned or cast to concrete numeric types.

```hop
fn main() {
	var signed i32    = -1
	var size   uint   = 10
	var text   &str   = "hej"
	var r      rune   = 'h'
	var p      rawptr = null
}
```

### Operators

HopHop has the usual arithmetic, bitwise, relational and logical operators. Assignment is an expression form, but the left side must be assignable.

Numeric conversions between concrete types are explicit. Casts use `as`, and pointer/reference casts through `rawptr` are the explicit low-level escape hatch.

```hop
fn main() {
	var a i32 = 10
	var b i64 = a as i64
	var ok    = a > 0 && b < 100
	var p     = null as rawptr
}
```

### Control flow

**if** conditions takes a `bool` conditional expression and splits execution into two branches

```hop
fn greet(name ?&str) {
	if name {
		print(name)
	} else {
		print("anonymous")
	}
}
```

When an "optional" value (`?T`) is used as the condition, its effective type is narrowed inside the branches. A non-null branch sees the payload type, while the null branch sees the null case.

**for** supports infinite loops, condition loops, C-style loops and `for ... in` iteration. `break` exits a loop or switch, and `continue` starts the next loop iteration.

The `for ... in` form can bind values, key/value pairs or discard values with `_`.

```hop
fn count(items &[i32]) i32 {
	var total = 0
	for i, value in items {
		total += i + value
	}
	return total
}
```

**switch** supports expression switches and condition switches. Cases are tested left-to-right, there is no fallthrough, and finite domains such as `bool` and enums must be exhaustive unless `default` is present.

Enum payload variants can be narrowed by switching on the enum value.

```hop
fn classify(n i32) str {
	switch {
		case n < 0  { return "negative" }
		case n == 0 { return "zero" }
		default     { return "positive" }
	}
}
```

### Error handling

`assert` checks a condition and traps if it fails. `panic` traps with a message and returns no value.

`defer` schedules a statement or block to run when the current scope exits through structured control flow such as fallthrough, `return`, `break` or `continue`.

```hop
fn use_value(x i32) {
	defer print("leaving")
	assert x >= 0, "expected non-negative"
	if x == 0 {
		panic("zero")
	}
}
```

### Arrays

Arrays have fixed length in the type. Slices are unsized views and must be used through pointer or reference forms when passed around.

`len` reports the length of strings, arrays and slices. Indexing and slicing use bracket syntax, and `copy(dst, src)` copies sequence elements.

```hop
fn first(xs &[i32]) i32 {
	assert len(xs) > 0
	return xs[0]
}

fn prefix(xs &[i32]) &[i32] {
	return xs[0:2]
}
```

### Pointers

`*T` is writable reference-like access to `T`.
`&T` is read-only reference-like access.

The address-of operator `&` forms a read-only reference, and unary `*` dereferences a pointer or reference. Slice mutability follows the wrapper: `*[T]` is writable, while `&[T]` is read-only.

```hop
fn read(x &i32) i32 {
	return *x
}

fn set(x *i32, value i32) {
	*x = value
}
```

### Optional

`?T` represents either a `T` or `null`.
A plain `T` can lift into `?T`, but an optional does not implicitly convert back to `T`.

Use control-flow narrowing for ordinary code and postfix `!` when an explicit runtime null trap is intended.

```hop
fn length(s ?str) int {
	if s == null {
		return 0
	}
	return len(s)
}
```

### Structs

`struct` groups named fields, and `union` stores one of several field layouts in the same storage. Fields can have defaults, and omitted struct fields are initialized from defaults or zero values.

Unions may initialize at most one field explicitly.

```hop
struct Point {
	x i32 = 0
	y i32 = 0
}

union Word {
	i i32
	u u32
}
```

### Enums

Enums have an integer base type, and enum items are scoped under the enum type. Plain enum values are selected as `Name.Item`.

Variants may carry payload fields. Payload constructors use compound-literal syntax, and switches can narrow payload variants for field access.

```hop
enum Result i32 {
    Ok{
        value i32
    }
    Err{
        code i32
    }
}

fn read(r Result) i32 {
    switch r {
        case Result.Ok as ok { return ok.value }
        case Result.Err      { return 0 }
    }
}
```

### Compound literals

Compound literals use named fields. A literal may name its type, or it may be inferred from an expected aggregate type.

Field names can be dotted for nested initialization. Explicit initializers override defaults for the initialized path.

```hop
struct Size {
	w i32
	h i32
}

fn main() {
	var size      = Size{ w: 640, h: 480 }
	var same Size = { w: 640, h: 480 }
}
```

### Type aliases

`type Name T` declares a distinct named type with `T` as its base. Assignment can implicitly peel from the alias to the target, but not from the target back to the alias.

Type names and value names live in separate namespaces, so a type and a function can share a spelling when their uses are unambiguous.

```hop
type UserId u64

fn raw(id UserId) u64 {
	return id
}
```

### Packages

A package is a file or directory; there is no `package` keyword. Imports appear before top-level declarations.

`pub` exports a top-level declaration. Imports can use the default alias, an explicit alias, a side-effect-only `_` alias or named symbol imports.

```hop
import "log" { Logger }
import "math" as m

pub fn area(r f64) f64 {
	return m.pi * r * r
}
```

### Function overloading

Functions may be overloaded by signature. Overload resolution ranks conversion costs deterministically and reports an ambiguity when there is no single best match.

For calls only, `recv.f(args...)` can resolve as `f(recv, args...)`. Real fields take precedence over selector-call sugar.

```hop
struct Cat {
    score int
}

struct Dog {
    score int
}

fn pick(v Cat) int {
    return v.score
}

fn pick(v Dog) int {
    return v.score
}

fn main() {
    cat := Cat{ score: 9 }
    dog := Dog{ score: 4 }
    assert pick(cat) == 9
    assert dog.pick() == 4
}
```

### Tuples

Tuple types and tuple-style result clauses represent multiple values.
Functions with tuple returns must return one value per position.

```hop
fn apply(f fn(i32) i32, x i32) i32 {
	return f(x)
}

fn divmod(a, b i32) (i32, i32) {
	return a / b, a % b
}
```

### Memory management

HopHop manual memory-management language. You control allocations and deallocations with `new` and `del`.

`new T` allocates memory, returning `*T`. `new [T n]` allocates an array of type `*[T]` (dynamically sized) or `*[T n]`, depending on the receiver type and if `n` can be computed at compile time or not.


```hop
fn make_count() *i32 {
	var p = new i32
	*p = 1
	return p
}

fn release(p *i32) {
	del p
}
```

`new` and `del` uses an allocator defined by `context.allocator` by default, and allows specifying an explicit allocator with `in`, e.g. `v := new [i32 3] in my_allocator`.

### Strings

`str` is UTF-8 text, and most string operations are byte-oriented.

```hop
fn main() {
	name := "hello" // type is &str
	print(msg)
}
```

`str` is a specialized type of `[u8]` that guarantees that its contents is valid UTF-8 text. An expression of type `&str` can be used wherever a value of type `&[u8]` or `&str` is needed, and an expression of type `*str` can be used as `*str`, `&str`, `*[u8]` and `&[u8]`. However, an expression of type `&[u8]` or `*[u8]` cannot be used as `&str` or `*str` without explicit cast. An explicit cast from `&[u8]` or `*[u8]` to `&str` or `*str` is the only way by which you can "break" the guarantee of a string containing valid UTF-8 data.

### Generics

Structs, unions, enums, type aliases and functions may declare type parameters with brackets after the declaration name. Type parameters are compile-time values of metatype `type`.

Named generic types are instantiated in type positions with type arguments. Generic function calls infer type arguments from ordinary arguments.

```hop
struct Box[T] {
	value T
}

fn get[T](box Box[T]) T {
	return box.value
}
```

### Types as values

Types can be treated as values. For example `i32.kind()` or `assert i64 != u32`. The "type" of a type is `type`, which together with compile-time evaluation allows declaring functions that take types as arguments and/or produces types.

The `type expr` syntax can be used in non-type contexts where syntax would otherwise be ambiguous, for example `&T` means "make a reference to expression T" in a value grammar position, while in a type position it means "reference type with base T". `type T` disambiguates to always mean "the type T".
`typeof(x)` returns the type of an expression.

Reflection helpers such as `kind`, `base`, `is_alias`, `type_name`, `ptr`, `slice` and `array` operate on type values.

```hop
fn main() {
	const T = type &[i32]
	const U = typeof(123)
	const P = ptr(U)
}
```

### Variadic parameters

A final parameter may be variadic with `name ...T`. In the body, concrete variadic parameters behave like a slice of `T`.

At the call site, arguments can be passed one by one or with a final spread argument using `...`.

```hop
fn sum(values ...i32) i32 {
	var total = 0
	for value in values {
		total += value
	}
	return total
}

fn main() {
	sum(1, 2, 3)
}
```

### Compile-time evaluation

A parameter marked `const` requires a const-evaluable argument at the call site. This lets library code validate values while typechecking the caller.

Const evaluation also powers constant numeric values, `sizeof`, compile-time function calls and type-value computations.

```hop
fn repeat(const n const_int, value i32) [i32 n] {
	var result [i32 n]
	return result
}
```

**`anytype`** is valid in function parameter positions and captures the static type of the corresponding argument. Each non-variadic `anytype` parameter binds independently.

`...anytype` forms a heterogeneous compile-time pack. Pack length is available with `len(args)`, and const indexes preserve the selected element type.

```hop
fn debug(value anytype) {
    print(type_name(typeof(value)))
}

fn count(args ...anytype) int {
    return len(args)
}
```

**`const { ... }`** executes a statement block in compile-time evaluation context. If it cannot be evaluated at compile time, compilation fails.

The `compiler` package provides diagnostic functions for code that validates itself during const evaluation.

```hop
import "compiler"

fn require_positive(const n const_int) {
	const {
		if n <= 0 {
			compiler.error("expected positive value")
		}
	}
}
```

### Anonymous aggregate types

Anonymous structs and unions can be written directly as types. Anonymous struct identity is structural, based on field names and field types.

This is useful for local shapes and context-like values that do not need a named declaration.

```hop
fn distance(p struct { x f64; y f64 }) f64 {
	return p.x*p.x + p.y*p.y
}

fn main() {
	distance({ x: 3.0, y: 4.0 })
}
```

### Struct composition

A struct may embed one named base struct as its first field. Direct fields win lookup, then promoted fields are searched through the embedded chain.

Embedded bases support implicit upcasts by value, pointer and reference forms.

```hop
struct Entity {
	id u64
}

struct User {
	Entity
	name str
}

fn entity_id(e &Entity) u64 {
	return e.id
}
```

### Variable-size structs

Variable-size structs use dependent fields whose length comes from a previous integer field. Once the first dependent field appears, following fields must also be dependent.

VSS values cannot be used by value in locals, parameters or returns. They are intended for pointer/reference layout work.

```hop
struct Packet {
	len  u32
	data [u8 .len]
}

fn packet_len(p &Packet) u32 {
	return p.len
}
```

### Context

`context` is an ambient builtin expression inside function bodies. The builtin `Context` provides at least `allocator`, `temp_allocator` and `logger`.

Operations such as `new`, `del`, `concat`, `fmt` and `print` use context capabilities when no explicit resource is supplied.

```hop
fn main() {
	var alloc = context.allocator
	var msg   = concat("hi, ", "there")
	print(msg)
	del msg in alloc
}
```

### Platform imports

`import "platform"` loads the builtin platform package. The platform surface is intentionally small and target dependent.

The common platform import exposes process/platform operations such as `exit(status i32)`.

```hop
import "platform"

fn main() {
	platform.exit(0)
}
```

### Iterator protocol

`for ... in` works over built-in sequence-like values and can also use the iterator protocol. A custom source provides `__iterator(x)` and matching `next_*` functions for the binding mode used by the loop.

This keeps the loop syntax simple while letting libraries define their own iteration shapes.

```hop
fn walk(xs &[i32]) i32 {
	var total = 0
	for value in xs {
		total += value
	}
	return total
}
```

### Directives and foreign linkage

Directives attach to the following top-level declaration. Current foreign-linkage directives include `@c_import`, `@wasm_import` and `@export`.

Imported declarations have no HopHop body because the symbol is supplied externally. `@export` publishes a public HopHop function under a requested external name.

```hop
@wasm_import("env", "now")
fn now() f64

@export("run")
pub fn run() {
	now()
}
```

### Build selection

Filename build tags let packages include files for specific targets. Active tags come from the selected backend and platform.

This keeps platform-specific code close to the package that needs it without adding conditional syntax inside the language.

```text
logger_wasm.hop
logger_macos.hop
logger_linux.hop
```

