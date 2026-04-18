// Verifies array type const expression is accepted.
const N = 1 + 2

fn f(x [i32 N]) i32 {
	return x[0]
}

fn main() {
	var a [i32 N]
	var b [i32 1 + 2 * 3]
	assert f(a) == 0
	assert b[0] == 0
}
