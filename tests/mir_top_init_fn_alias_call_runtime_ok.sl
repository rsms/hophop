fn add1(x i32) i32 {
	return x + 1
}

var f = add1

var x = f(41)

fn main() {
	assert x == 42
}
