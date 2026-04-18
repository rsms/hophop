// Verifies multi return return arity is rejected.
fn bad() (uint, uint) {
	return 1
}

fn main() {
	_ = bad()
}
