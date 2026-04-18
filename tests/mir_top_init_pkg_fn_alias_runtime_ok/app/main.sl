// Supports MIR top-level initialization package function alias runtime by providing the app entrypoint.
import "lib/dupe" as dupe

fn helper() i32 {
	return 7
}

var f = helper

var x i32 = f()

fn main() {
	assert dupe.helper() == 99
	assert x == 7
}
