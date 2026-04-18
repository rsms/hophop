// Supports const block MIR package function value by providing the app entrypoint.
import "lib/math"

fn main() {
	const {
		const f = math.Double
		assert f(21) == 42
	}
}
