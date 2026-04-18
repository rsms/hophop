// Verifies the Wasm backend accepts break.
fn main() i32 {
	var x i32 = 0
	for x = 0; x < 5; x += 1 {
		if x == 3 {
			break
		}
	}
	return x
}
