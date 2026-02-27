fn m(a int) int {
	return a + 10
}

fn m(a int, b int) int {
	return a + b
}

fn main() {
	assert m(1) == 11
	assert m(1, b: 2) == 3
}
