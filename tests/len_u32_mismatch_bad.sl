// Verifies len u32 mismatch is rejected.
fn main() {
	var xs [i32 3]
	var n  u32 = len(xs)
	_ = n
}
