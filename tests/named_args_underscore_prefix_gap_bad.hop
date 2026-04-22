// Verifies unlabeled fixed arguments are rejected after the underscore prefix.
fn f(a, _b, c, _d int) int {
	return a + _b + c + _d
}

fn main() {
	var x = f(1, 2, 4, c: 3)
	_ = x
}
