// grouped declarations, short declarations and multi-assignment
const TOP_A, TOP_B = 2, 3

fn pair() (int, int) {
	return 7, 8
}

fn main() {
	// declare multiple names with one type
	var u, v, w f64
	assert u == 0.0 && v == 0.0 && w == 0.0

	// short declarations infer new mutable locals
	x, y := 1 as u32, 2 as u32
	re, im := 0.0, 1.0
	assert re == 0.0 && im == 1.0

	// short declarations can mix assignment and declaration
	x, z := y, 3 as u32
	assert x == 2 && y == 2 && z == 3

	// a single tuple RHS can be decomposed
	first, second := pair()
	assert first == 7 && second == 8

	// all-existing short assignment behaves like assignment
	x, y := y + z, x
	assert x == 5 && y == 2

	// swap values
	x, y = y, x
	assert x == 2 && y == 5

	// multi-assignment evaluates RHS first, then stores left-to-right
	var a [int 6]
	a[x], x = 3, 4
	assert a[2] == 3 && x == 4
	x, a[x] = 5, 6
	assert x == 5 && a[5] == 6

	// grouped const also works locally
	const c, d = 4, 5
	assert c + d == TOP_A + TOP_B + 4
}
