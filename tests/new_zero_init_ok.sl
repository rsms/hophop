// Verifies new zero initialization is accepted.
struct S {
	x int
	y [i32 2]
}

fn main() {
	var ma         = context.mem
	var p *S       = new S with ma
	var q *[i32 4] = new [i32 4] with ma

	assert p.x == 0
	assert p.y[0] == 0
	assert p.y[1] == 0

	assert q[0] == 0
	assert q[1] == 0
	assert q[2] == 0
	assert q[3] == 0
}
