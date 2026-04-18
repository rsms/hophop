// Verifies const local grouped initializer nonconst is rejected.
fn f(x i32) {
	const a, b = 1, x
	_ = a
	_ = b
}

fn main() {}
