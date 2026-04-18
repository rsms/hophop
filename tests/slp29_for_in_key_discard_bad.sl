// Verifies SLP 29 for in key discard is rejected.
fn main() {
	var a [i32 2]
	for _, v in &a {
		assert v >= 0
	}
}
