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

const VALUE = make_pair().left + make_pair().right

fn main() {
	assert VALUE == 10
}
