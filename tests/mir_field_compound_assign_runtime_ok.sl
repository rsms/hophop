// Verifies MIR runtime behavior for field compound assign.
struct Pair {
	a i32
	b i32
}

fn bump_a(p *Pair) {
	p.a += 5
}

fn main() {
	var p = Pair{}
	p.a = 4
	bump_a(&p)
	assert p.a == 9
}
