// Verifies unlabeled fixed arguments are rejected after named suffix arguments begin.
fn f(a, _b, c, _d int) int {
	return a + _b + c + _d
}

fn main() {
	var x = f(1, 2, c: 3, 4)
	_ = x
}
