// Verifies struct field defaults order is accepted.
var g i32 = 0

fn step(v i32) i32 {
	g = g*10 + v
	return v
}

struct S {
	a i32
	b i32 = step(a + 2)
	c i32 = step(b + 3)
}

fn main() {
	var s = S{ c: step(9), a: step(1) }

	assert (g == 913)
	assert (s.a == 1)
	assert (s.b == 3)
	assert (s.c == 9)
}
