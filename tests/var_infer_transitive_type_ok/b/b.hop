// Supports variable inference transitive type by providing the b package fixture.
import "a"

pub struct B {
	a a.A
}

pub fn make_a() a.A {
	var v a.A
	v.x = 7
	return v
}

pub fn make_b() B {
	var v B
	v.a = make_a()
	return v
}
