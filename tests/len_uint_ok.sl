// Verifies len uint is accepted.
fn main() {
	var xs [i32 3]
	var n  uint = len(xs)
	assert n == 3
}
