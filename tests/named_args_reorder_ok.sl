fn build(a i32, b i32, c i32) i32 {
	return a * 100 + b * 10 + c
}

fn main() {
	var a i32 = 1
	var c i32 = 3
	var b i32 = 2
	assert build(a, c, b) == 123
}
