// Verifies explicit allocator syntax for new and del.
fn main() {
	var ma     = context.allocator
	var a *i32 = new i32 in ma
	var b *i32 = new i32 in ma
	*a = 10
	*b = 32
	assert *a + *b == 42
	del a, b in ma
}
