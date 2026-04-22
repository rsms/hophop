// Verifies new nonvss implicit initialization defaults is accepted.
struct S {
	a i32 = 7
	b i32 = a + 2
}

fn main() {
	var p *S = new S
	var q *S = new S{}

	assert p.a == 7
	assert p.b == 9
	assert q.a == 7
	assert q.b == 9
}
