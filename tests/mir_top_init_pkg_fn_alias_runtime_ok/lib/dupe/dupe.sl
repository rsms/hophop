// Supports MIR top-level initialization package function alias runtime by providing the dupe library package.
pub fn helper() i32

fn helper() i32 {
	return 99
}

var f = helper
