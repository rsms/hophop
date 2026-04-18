// Supports const-eval package function value function body by providing the app entrypoint.
import "lib/math"

fn answer() int {
	const f = math.Double
	return f(21)
}

fn main() {
	assert answer() == 42
	const {
		assert answer() == 42
	}
}
