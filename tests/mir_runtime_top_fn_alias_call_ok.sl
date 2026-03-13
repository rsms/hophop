fn add1(x i32) i32 {
	return x + 1
}

var f = add1

fn main() {
	assert f(41) == 42
}
