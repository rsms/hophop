// SLP-12 typed contexts and call-site overlays.
// Demonstrates:
// - function context clauses
// - implicit context forwarding
// - explicit pass-through (`with context`)
// - call-local overlays (`with { ... }`)
// - contextual built-ins (`new T` and `print(...)`)
// Context types can be named as they are simply struct types
struct AppContext {
	mem *Allocator
	log Logger
}

fn alloc_value() *i32 context struct {
	mem *Allocator
} {
	var p *i32 = new i32
	*p = 42
	return p
}

fn announce(msg &str) context struct {
	mem *Allocator
	log Logger
} {
	print(msg)
}

fn run_once() i32 context AppContext {
	// Implicit forwarding from caller context -> callee context (field subset by name)
	var p *i32 = alloc_value()

	announce("implicit")

	announce("explicit") with context

	// Call-local overlay, with explicit and shorthand binds.
	var p2 *i32 = alloc_value() with { mem = context.mem }
	announce("overlay") with { log, mem }

	assert *p == 42
	assert *p2 == 42
	return *p + *p2
}

fn main() {
	// `main` can supply call-local capabilities explicitly.
	var a = run_once() with { mem = context.mem, log = context.log }
	var b = run_once() with { mem = context.mem, log = context.log }

	assert a == 84
	assert b == 84
} // Ordinary calls auto-forward current context.
// Explicit pass-through (equivalent to omitting `with`).
