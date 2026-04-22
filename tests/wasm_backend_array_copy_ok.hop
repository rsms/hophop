// Verifies the Wasm backend accepts array copy.
fn main() {
	var a [i32 3]
	var b [i32 3]
	a[0] = 7
	b = a
	assert b[0] == 7
	assert len(b[:]) == 3
}
