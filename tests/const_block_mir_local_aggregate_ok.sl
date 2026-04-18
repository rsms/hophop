// Verifies const block MIR local aggregate is accepted.
struct Pair {
	left  int
	right int
}

fn next() int {
	const {
		var p Pair
		p.left = 7
		p.right = 3
		assert p.left + p.right == 10
	}
	return 1
}

fn main() {
	assert next() == 1
}
