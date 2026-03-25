import "lib/math"

fn answer() int {
	return math.Double(21)
}

fn main() {
	const {
		assert answer() == 42
	}
}
