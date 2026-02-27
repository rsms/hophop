struct Vec2 {
	x i32
	y i32
}

fn main() {
	var a Vec2 = { x: 1, y: 2 }
	a = { x: 3, y: 4 }
	assert (a.x == 3)
	assert (a.y == 4)
}
