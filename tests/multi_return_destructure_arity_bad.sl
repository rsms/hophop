// Verifies multi return destructure arity is rejected.
fn pair() (uint, uint) {
	return 1, 2
}

fn main() {
	var a, b, c = pair()
	_ = a
	_ = b
	_ = c
}
