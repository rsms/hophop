// Verifies the Wasm backend accepts aggregate nested.
struct Inner {
	x i32
	y i32
}

struct Outer {
	a Inner
	b Inner
}

fn main() {
	var o = Outer{ a: Inner{ x: 1, y: 2 } }
	assert o.a.x == 1
}
