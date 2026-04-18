// Verifies MIR runtime behavior for assert.
fn main() {
	var x, y u32 = 1, 2
	assert x == 1
	assert y == 2
}
