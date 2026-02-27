struct Vec2 {
	x i32
	y i32
}

fn round(v *Vec2) {}

fn main() {
	round({ x: 1, y: 2 })
}
