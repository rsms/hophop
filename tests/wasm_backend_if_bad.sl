// Verifies the Wasm backend rejects if.
fn main() i32 {
	if true {
		return 1
	}
	return 0
}
