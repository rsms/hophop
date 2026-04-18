// Verifies the Wasm backend accepts len.
fn main() i32 {
	var a &str = "abc"
	var s *str = "de"

	assert len(a) == 3
	assert len(s) == 2

	return 5
}
