// Supports top-level const package call by providing the app entrypoint.
import "lib/math"

const VALUE = math.Double(21)

fn main() {
	assert VALUE == 42
}
