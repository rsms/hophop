fn add1(x int) int {
	return x + 1
}

fn main() {
	const {
		const f = add1
		assert f(41) == 42
	}
}
