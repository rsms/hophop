// Verifies new array type mismatch.
fn foo(ma *Allocator) {
	var many *i32 = new [i32 8] context ma
	_ = many
}
