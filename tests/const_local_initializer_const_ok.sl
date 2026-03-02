fn f() int {
	const y = 1 + 2
	return y
}

fn main() {
	assert f() == 3
}
