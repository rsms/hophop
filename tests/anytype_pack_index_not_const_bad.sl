// Verifies anytype pack index not const is rejected.
fn bad(args ...anytype) {
	var i u32 = 0
	_ = args[i]
}

fn main() {
	bad(1)
}
