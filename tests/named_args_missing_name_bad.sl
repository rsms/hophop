// Verifies named arguments missing name is rejected.
fn build(a, b, c i32) i32 {
	return a + b + c
}

fn main() {
	var x = build(1, 2, c: 3)
	_ = x
}
