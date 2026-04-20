// Verifies formatter can drop literal casts from imported function call arguments.
import "input/lib/math"

fn main() {
	_ = math.sum(1, b: 2, c: 3.5)
}
