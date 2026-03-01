type MyInt int

fn a(x int) int {
	return x + 1
}

fn main() {
	var v MyInt = 3
	assert v.a() == 4
	assert a(v) == 4
}
