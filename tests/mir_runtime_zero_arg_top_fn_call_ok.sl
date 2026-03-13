fn forty_two() i32 {
	return 42
}

var f = forty_two

fn main() {
	assert f() == 42
}
