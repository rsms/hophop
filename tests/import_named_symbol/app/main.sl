// Supports import named symbol by providing the app entrypoint.
import "lib/math" { Double }

fn main() i32 {
	return Double(3)
}
