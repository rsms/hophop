fn foo(ma *Allocator) {
	var many *i32 = new [i32 8] with ma
	_ = many
}
