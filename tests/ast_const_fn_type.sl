// Verifies the AST for const function type.
struct Allocator {
	impl fn(*Allocator, rawptr, uint, uint, *uint, u32) rawptr
}

fn takes(cb fn(const i32, const &str, const ...i32) bool) fn(const i32) i32 {
	var local fn(const i32) i32
	return local
}
