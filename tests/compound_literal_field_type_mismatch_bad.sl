// Verifies compound literal field type mismatch is rejected.
struct Vec2 {
	x i32
	y i32
}

fn main() {
	var v Vec2 = { x: "x", y: 1 }
	_ = v
}
