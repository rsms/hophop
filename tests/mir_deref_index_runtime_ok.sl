// Verifies MIR runtime behavior for dereference index.
fn bump_second(ptrs *[*i32 2]) {
	*ptrs[1] =  9
	*ptrs[1] += 5
}

fn main() {
	var first  i32 = 0
	var second i32 = 1
	var ptrs   [*i32 2]
	ptrs[0] = &first
	ptrs[1] = &second
	bump_second(&ptrs)
	assert second == 14
}
