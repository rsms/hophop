import "foo"

struct A {
	x int
}

fn pick(v A) int {
	return v.x
}

fn pick(v foo.B) int {
	return foo.pick_b(v)
}

fn main() {
	var a A
	var b foo.B

	a.x = 7
	b.y = 11

	assert pick(a) == 7
	assert pick(b) == 11
}
