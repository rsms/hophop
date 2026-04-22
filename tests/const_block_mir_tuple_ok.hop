// Verifies const block MIR tuple is accepted.
fn pair(x, y int) (int, int) {
	return x + 1, y + 2
}

fn next() int {
	const {
		var a, b = pair(2, y: 3)
		assert a == 3
		assert b == 5
	}
	return 2
}

const VALUE = next()

fn main() {
	assert VALUE == 2
}
