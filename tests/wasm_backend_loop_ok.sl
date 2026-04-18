// Verifies the Wasm backend accepts loop.
fn main() i32 {
	var x i32 = 0
	for x < 3 {
		x += 1
	}
	return x
}
