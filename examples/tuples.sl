// tuple types, tuple expressions, and multi-value returns
fn time_of_day() (uint, uint, uint) {
	return 14, 25, 30
}

fn shift_xy(x int, y int, dx int, dy int) (int, int) {
	return x + dx, y + dy
}

fn accepts_point(p (int, int)) {
	_ = p
}

fn main() {
	// tuple-typed variable from tuple expression
	var tagged (i32, bool) = (123, true)
	_ = tagged

	// function multi-return decomposition
	var h, m, s = time_of_day()
	assert h == 14 && m == 25 && s == 30

	// tuple values can be named with an explicit tuple type
	var hms (uint, uint, uint) = time_of_day()
	_ = hms

	// multi-assignment from multi-return call
	h, m, s = time_of_day()
	assert h == 14 && m == 25 && s == 30

	// tuple return values with arithmetic
	var px, py = shift_xy(10, y: 20, dx: -3, dy: 7)
	assert px == 7 && py == 27
	px, py = shift_xy(px, y: py, dx: 1, dy: 2)
	assert px == 8 && py == 29

	accepts_point((px, py))
} // tuple literals can be passed to tuple-typed params
