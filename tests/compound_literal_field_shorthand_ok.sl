// Verifies compound literal field shorthand is accepted.
struct Pair {
	x i32
	y i32
}

fn main() {
	var y i32  = 7
	var p Pair = { x: 1, y }
	assert p.x == 1
	assert p.y == 7
}
