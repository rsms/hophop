// Supports MIR package call runtime by providing the app entrypoint.
import "lib/math"

fn main() {
	var x = math.Double(21)
	assert x == 42
}
