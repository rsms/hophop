// Supports MIR package function alias runtime by providing the dupe library package.
pub fn helper(x i32) i32

fn helper(x i32) i32 {
	return 99
}

var f = helper
