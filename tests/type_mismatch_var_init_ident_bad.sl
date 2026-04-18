// Verifies type mismatch variable initialization identifier is rejected.
fn main() {
	var a i32 = 123
	var b i64 = a
}
