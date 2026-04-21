// Verifies the Wasm backend accepts new aggregate initialization.
struct Pair {
	x i32
	y i32
}

fn main() {
	var ma      = context.mem
	var p *Pair = new Pair{ x: 1, y: 2 } context ma
	assert p.x == 1
	assert p.y == 2
}
