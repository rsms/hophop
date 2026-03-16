import "lib/math"

fn answer() int {
	var f = math.Double
	return f(21)
}

fn main() {
	assert answer() == 42
}
