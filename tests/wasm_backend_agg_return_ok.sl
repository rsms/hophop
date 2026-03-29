struct Pair {
	x i32
	y i32
}

fn mk() Pair {
	return Pair{ x: 1, y: 2 }
}

fn main() {
	var p = mk()
	assert p.x == 1
}
