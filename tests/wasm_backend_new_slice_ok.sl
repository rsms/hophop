// Verifies the Wasm backend accepts new slice.
fn main() {
	var n  uint   = 4
	var xs *[i32] = new [i32 n]
	assert len(xs) == 4
}
