struct A {
	x int = 1
}

struct B {
	a A
}

struct C {
	s struct {
		x int = 1
	}
}

fn main() {
	var c C = { s = {} }
	assert (c.s.x == 1)
}
