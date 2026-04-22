// Verifies the Wasm backend accepts new aggregate initialization.
struct Pair {
	x i32
	y i32
}

fn main() {
	var p *Pair = new Pair{ x: 1, y: 2 }
	assert p.x == 1
	assert p.y == 2
}
