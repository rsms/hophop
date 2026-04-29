// Verifies unqualified builtin types lower through the C backend.
fn keep_allocator(a *Allocator, l *Logger, c *Context) *Allocator {
	if l == (null as rawptr) as *Logger {
		return a
	}
	if c == (null as rawptr) as *Context {
		return a
	}
	return a
}

fn main() {
	var a   *Allocator = (null as rawptr) as *Allocator
	var l   *Logger    = (null as rawptr) as *Logger
	var c   *Context   = (null as rawptr) as *Context
	var out *Allocator = keep_allocator(a, l, c)
	assert out == a
}
