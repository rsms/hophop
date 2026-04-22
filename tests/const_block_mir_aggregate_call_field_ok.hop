// Verifies const block MIR aggregate call field is accepted.
struct Pair {
	left  int
	right int
}

fn make_pair() Pair {
	var p Pair
	p.left = 7
	p.right = 3
	return p
}

fn main() {
	const {
		assert make_pair().left + make_pair().right == 10
	}
	assert make_pair().left == 7
}
