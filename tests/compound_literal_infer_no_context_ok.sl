// Verifies compound literal inference no context is accepted.
struct Vec2 {
	x i32
	y i32
}

fn main() {
	var v = { x: 1, y: 2 }
	assert (v.x == 1)
	assert (v.y == 2)
}
