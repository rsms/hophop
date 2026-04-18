// Supports import symbols and alias by providing the app entrypoint.
import "lib/math" as m { Double }

fn main() i32 {
	return m.Double(10) + Double(1)
}
