fn forty_two() i32 {
	return 42
}

var f = forty_two

var x = f()

fn main() {
	assert x == 42
}
