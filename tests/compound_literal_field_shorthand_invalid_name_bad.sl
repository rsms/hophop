// Verifies compound literal field shorthand invalid name is rejected.
struct Pair {
	x i32
	y i32
}

fn main() {
	var z i32  = 8
	var p Pair = { z }
	_ = p
}
