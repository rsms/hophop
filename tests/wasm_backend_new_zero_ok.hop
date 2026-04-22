// Verifies the Wasm backend accepts new zero.
struct S {
	x int
	y [i32 2]
}

fn main() {
	var p *S       = new S
	var q *[i32 4] = new [i32 4]

	p.x = 7
	q[0] = 9

	assert p.x == 7
	assert p.y[1] == 0
	assert q[0] == 9
	assert q[1] == 0
}
