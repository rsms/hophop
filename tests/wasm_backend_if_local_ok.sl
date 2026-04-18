// Verifies the Wasm backend accepts if local.
fn main() i32 {
	var x i32
	if true {
		x = 1
	} else {
		x = 2
	}
	return x
}
