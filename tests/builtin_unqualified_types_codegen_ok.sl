// Verifies unqualified builtin types lower through the C backend.
fn keep_allocator(a *MemAllocator, l *Logger, c *Context) *MemAllocator {
	if l == (null as rawptr) as *Logger {
		return a
	}
	if c == (null as rawptr) as *Context {
		return a
	}
	return a
}

fn main() {
	var a   *MemAllocator = (null as rawptr) as *MemAllocator
	var l   *Logger       = (null as rawptr) as *Logger
	var c   *Context      = (null as rawptr) as *Context
	var out *MemAllocator = keep_allocator(a, l, c)
	assert out == a
}
