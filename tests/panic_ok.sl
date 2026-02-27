pub fn must_positive(n i32) i32 {
	if n <= 0 {
		panic("n must be positive")
	}
	return n
}
