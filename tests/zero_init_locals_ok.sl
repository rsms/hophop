struct A {
	x int
}

struct B {
	a A
}

fn main() {
	var x int
	var b B
	assert x == 0
	assert b.a.x == 0
}
