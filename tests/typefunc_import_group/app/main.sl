import "foo"

struct A {
	x int
}

fn pick_a(v A) int {
	return v.x
}

fn pick { pick_a, foo.pick_b }

fn main() {
	var a A
	var b foo.B

	a.x = 7
	b.y = 11

	assert pick(a) == 7
	assert pick(b) == 11
}
