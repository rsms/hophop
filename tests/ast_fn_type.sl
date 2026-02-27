struct Allocator {
	impl fn(*Allocator, uint, uint, uint, *uint, u32) uint
}

fn takes(cb fn(i32, &str) bool, plain fn()) fn(i32) i32 {
	var local fn(i32, i32) i32
	return local
}
