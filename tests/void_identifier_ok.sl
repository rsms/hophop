// Verifies void identifier is accepted.
struct void {
	x i32
}

fn main() {
	var v void = void{ x: 7 }
	assert v.x == 7
}
