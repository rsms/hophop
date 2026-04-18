// Verifies type function overload is accepted.
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

fn score(v A) int {
	return v.x
}

fn score(v B) int {
	return v.y
}

fn main() {
	var a A
	var b B
	var c C

	a.x = 1

	b.x = 2
	b.y = 3

	c.x = 4
	c.y = 5
	c.z = 6

	assert score(a) == 1
	assert score(b) == 3
	assert score(c) == 5

	assert a.score() == 1
	assert b.score() == 3
	assert c.score() == 5
}
