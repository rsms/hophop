// Verifies leading-underscore parameters extend the positional call prefix.
fn rgb32(_r, _g, _b int) int {
	return _r*100 + _g*10 + _b
}

fn mixed(a, _b, c int) int {
	return a*100 + _b*10 + c
}

fn reordered(_a, _b, c, _d int) int {
	return _a*1000 + _b*100 + c*10 + _d
}

const MIXED_VALUE = mixed(1, 2, c: 3)

const REORDERED_VALUE = reordered(1, 2, _d: 4, c: 3)

fn main() {
	assert rgb32(1, 3, _b: 2) == 132
	assert rgb32(_r: 1, _g: 3, _b: 2) == 132
	assert mixed(1, 2, c: 3) == 123
	assert reordered(1, 2, _d: 4, c: 3) == 1234
	assert MIXED_VALUE == 123
	assert REORDERED_VALUE == 1234
}
