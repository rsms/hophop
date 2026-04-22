// Supports MIR package qualified value runtime by providing the app entrypoint.
import "lib/dupe" as dupe

fn imported_fn_value() i32 {
	var f = dupe.helper
	return f(5)
}

fn main() {
	assert imported_fn_value() == 10
}
