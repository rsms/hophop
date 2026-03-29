struct Pair {
	x i32
	y i32
}

fn main() {
	var p      = Pair{ x: 2 }
	var q Pair = { x: 7, y: 9 }
	q = { x: p.x + 3 }
	assert p.x == 2
	assert p.y == 0
	assert q.x == 5
	assert q.y == 0
}
