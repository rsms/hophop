fn forty_two() int {
	return 42
}

fn main() {
	const f = forty_two
	assert 1 + f() == 43
}
