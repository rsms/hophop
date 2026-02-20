// SLP-12 typed contexts and call-site overlays.
// Demonstrates:
// - function context clauses
// - implicit context forwarding
// - explicit pass-through (`with context`)
// - call-local overlays (`with { ... }`)
// - contextual built-ins (`new(T)` and `print(...)`)
import "std/mem"

struct AllocContext {
    mem mut&mem.Allocator
}

struct AppContext {
    mem     mut&mem.Allocator
    console u64
}

fn alloc_value() *i32 context AllocContext {
    var p *i32 = new(i32)
    *p = 42
    return p
}

fn announce(msg str) context AppContext {
    print(msg)
}

fn run_once() i32 context AppContext {
    // Implicit forwarding from AppContext -> AllocContext (field subset by name)
    var p *i32 = alloc_value()

    // Ordinary calls auto-forward current context.
    announce("implicit")

    // Explicit pass-through (equivalent to omitting `with`).
    announce("explicit") with context

    // Call-local overlay, with explicit and shorthand binds.
    var p2 *i32 = alloc_value() with { mem = context.mem }
    announce("overlay") with { console, mem }

    assert *p == 42
    assert *p2 == 42
    return *p + *p2
}

fn main() {
    // `main` can supply call-local capabilities explicitly.
    var a = run_once() with { mem = mem.platformAllocator, console = 0 }
    var b = run_once() with { mem = mem.platformAllocator, console = 0 }

    assert a == 84
    assert b == 84
}
