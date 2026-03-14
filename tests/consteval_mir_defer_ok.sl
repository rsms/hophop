fn next(x int) int {
	var y int = x
	defer y += 3
	return y
}

const VALUE int = next(2)

fn main() {
	assert VALUE == 5
}
