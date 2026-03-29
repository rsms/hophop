struct Inner {
	x i32
	y i32
}

struct Outer {
	a Inner
	b Inner
}

fn main() {
	var o Outer = Outer{ a: Inner{ x: 1, y: 2 } }
	assert o.a.x == 1
}
