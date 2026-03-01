// nominal alias type with one-way implicit conversion to target type
type MyInt int

fn score(x MyInt) int {
	return x as int + 1
}

fn score(x int) int {
	return x + 2
}

fn main() {
	var v MyInt = 3 as MyInt

	assert score(v) == 4
	assert v.score() == 4
	assert score(3) == 5
}
