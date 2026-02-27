struct Point {
	x i32
	y i32
}

fn main() {
	var p = Point{ x: 10, y: 20 }
	assert (p.x == 10)
	assert (p.y == 20)
}
