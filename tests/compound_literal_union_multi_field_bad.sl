union U {
	a i32
	b i32
}

fn main() {
	var u U = { a: 1, b: 2 }
	_ = u
}
