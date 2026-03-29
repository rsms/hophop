struct Pair {
	x i32
	y i32
}

fn main() {
	var ma      = context.mem
	var p *Pair = new Pair{ x: 1, y: 2 } with ma
	assert p.x == 1
	assert p.y == 2
}
