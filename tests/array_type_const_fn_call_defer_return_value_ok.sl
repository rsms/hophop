// Verifies array type const function call defer return value is accepted.
fn next(x i32) i32 {
	var y = x
	defer y += 3
	return y
}

fn main() {
	var a [i32 next(2)]
	assert a[0] == 0
}
