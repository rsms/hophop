// Supports MIR top-level initialization helper package call runtime by providing the app entrypoint.
import "lib/math"

fn helper() int {
	return math.Double(21)
}

var value int = helper()

fn main() {
	assert value == 42
}
