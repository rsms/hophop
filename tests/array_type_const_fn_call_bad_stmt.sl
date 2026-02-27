fn next(x i32) i32 {
	defer return x
	return x
}

fn main() {
	var a [i32 next(2)]
	assert a[0] == 0
}
