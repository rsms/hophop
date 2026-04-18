// Verifies pointer embedded upcast is accepted.
struct A {
	x i32
}

struct B {
	A
	y i32
}

fn bump(base *A) {
	base.x += 1
}

fn main() {
	var b B
	var p *A = &b
	p.x = 41
	bump(&b)
	assert b.x == 42
}
