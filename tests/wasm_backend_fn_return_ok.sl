fn add1(x i32) i32 {
	return x + 1
}

fn pick() fn(i32) i32 {
	return add1
}

fn main() i32 {
	var f = pick()
	return f(41)
}
