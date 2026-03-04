fn next(const n int) int {
	const {
		const m int = n + 1
		assert m > n
	}
	return n + 1
}

fn main() {
	assert next(2) == 3
}
