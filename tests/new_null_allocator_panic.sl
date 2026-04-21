// Verifies new panics when its allocator argument is null.
fn main() {
	var ma *Allocator = (null as rawptr) as *Allocator
	var p  ?*i32      = new i32 context ma
	assert p == null
}
