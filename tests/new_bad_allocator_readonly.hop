// Verifies new bad allocator read-only.
fn main() i32 {
	var ma MemAllocator
	var ro &MemAllocator = &ma
	var p  *i32          = new i32 in ro
	return p as i32
}
