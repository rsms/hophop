type MyInt int

fn a(x MyInt) int {
	return x as int + 1
}

fn a(x int) int {
	return x + 2
}

fn main() {
	var v MyInt = 3
	assert a(v) == 4
	assert v.a() == 4
	assert a(3) == 5
}
