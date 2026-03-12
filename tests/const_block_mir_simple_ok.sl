fn next(const n int) int {
	const {
		const m int = n + 1
		if m > 2 {
			assert m == 3
		}
	}
	return n + 1
}

fn main() {
	assert next(2) == 3
}
