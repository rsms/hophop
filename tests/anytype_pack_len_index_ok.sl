// Verifies anytype pack len index is accepted.
fn first_i32(args ...anytype) int {
	assert len(args) == 3
	var x int = args[0]
	_ = args[1]
	_ = typeof(args[2])
	return x
}

fn main() {
	assert first_i32(7, true, "ok") == 7
}
