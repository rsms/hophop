fn next() int {
	const {
		var y int = 2
		defer assert y == 5
		y += 3
	}
	return 1
}

fn main() {
	assert next() == 1
}
