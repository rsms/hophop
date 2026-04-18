// Supports const block MIR package call by providing the app entrypoint.
import "lib/math"

fn answer() int {
	return math.Double(21)
}

fn main() {
	const {
		assert answer() == 42
	}
}
