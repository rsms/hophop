// Verifies the Wasm backend accepts new slice aggregate.
struct Pair {
	a i32
}

fn main() {
	var n  uint    = 4
	var xs *[Pair] = new [Pair n]
	assert len(xs) == 4
	xs[0].a = 7
	assert xs[0].a == 7
}
