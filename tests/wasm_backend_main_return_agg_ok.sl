// Verifies the Wasm backend accepts main return aggregate.
struct Pair {
	x i32
	y i32
}

fn main() Pair {
	var p = Pair{ x: 1, y: 2 }
	assert p.x == 1
	assert p.y == 2
	return p
}
