const A, B = 2, 3

fn main() {
	var u, v, w f64
	var x, y u32 = 1, 2
	const c, d = 4, 5
	var re, im = 0.0, 1.0
	var _, found = 123, true
	x, y = y, x

	_ = 1
	_ = true
	_ = 1.5

	assert A == 2 && B == 3
	assert c == 4 && d == 5
	assert u == 0.0 && v == 0.0 && w == 0.0
	assert re == 0.0 && im == 1.0
	assert x == 2 && y == 1 && found
}
