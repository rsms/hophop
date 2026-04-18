// Verifies MIR runtime behavior for len.
fn main() {
	var bytes [u8 3]
	assert len(bytes) == 3
	assert len("abcd") == 4
}
