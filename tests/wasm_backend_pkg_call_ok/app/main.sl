// Supports Wasm backend package call by providing the app entrypoint.
import "lib/math"

fn main() i32 {
	return math.Double(21)
}
