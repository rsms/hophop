fn add1(x int) int {
	return x + 1
}

fn add2(x int) int {
	return add1(x) + 1
}

var value int = add2(5)

fn main() {
	assert value == 7
}
