// Verifies new array type mismatch.
fn foo(ma *MemAllocator) {
	var many *i32 = new [i32 8]
	_ = many
}
