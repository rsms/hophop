// Verifies MIR runtime behavior for multi return.
fn pair(x, y int) (int, int) {
	return x + 1, y + 2
}

fn accepts_point(p (int, int)) {
	_ = p
}

fn main() {
	var x, y = pair(10, y: 20)
	assert x == 11 && y == 22

	x, y = pair(x, y)
	assert x == 12 && y == 24

	var xy (int, int) = pair(x, y)
	_ = xy

	accepts_point((x, y))
}
