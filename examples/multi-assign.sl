// grouped declarations and multi-assignment
const TOP_A, TOP_B = 2, 3

fn main() {
	// declare multiple names with one type
	var u, v, w f64
	assert u == 0.0 && v == 0.0 && w == 0.0

	// declare and initialize multiple names at once
	var x, y u32 = 1, 2
	var re, im = 0.0, 1.0
	assert re == 0.0 && im == 1.0

	// swap values
	x, y = y, x
	assert x == 2 && y == 1

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
