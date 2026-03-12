fn fact(n i32) i32 {
	if n <= 1 {
		return 1
	}
	return n * fact(n - 1)
}

fn main() {
	assert fact(5) == 120
}
