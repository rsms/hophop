fn pick(const x i32) i32 {
	return 1
}

fn pick(x i32) i32 {
	return 2
}

fn main() {
	var n i32 = 1
	assert pick(n) == 2
}
