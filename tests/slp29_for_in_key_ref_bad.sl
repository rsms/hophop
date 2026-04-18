// Verifies SLP 29 for in key reference is rejected.
fn main() {
	var a [i32 2]
	for &i, v in &a {
		assert i >= 0
		assert v >= 0
	}
}
