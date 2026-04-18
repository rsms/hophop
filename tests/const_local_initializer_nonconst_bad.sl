// Verifies const local initializer nonconst is rejected.
fn f(x i32) {
	const y = x + 1
	_ = y
}

fn main() {}
