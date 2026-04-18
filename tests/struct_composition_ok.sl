// Verifies struct composition is accepted.
struct A {
	x int
}

struct B {
	A
	y int
}

struct C {
	B
	z int
}

fn take_A(v A) int {
	return v.x
}

fn bump_A(v *C) {
	v.x = v.x + 1
}

fn main() {
	var b B
	var c C

	c.x = 1
	c.y = 2
	c.z = 3

	b = c

	assert b.x == 1
	assert b.y == 2
	assert take_A(c) == 1

	bump_A(&c)
	assert c.x == 2
}
