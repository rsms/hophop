// Verifies the Wasm backend accepts array return.
fn mk() [i32 3] {
	var a [i32 3]
	return a
}

fn main() {
	var b = mk()
	assert len(b[:]) == 3
}
