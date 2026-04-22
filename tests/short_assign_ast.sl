// Verifies AST for short variable declarations.
fn pair() (int, int) {
	return 1, 2
}

fn main() {
	x := 1
	x, y, z := 2, 3 as i32, 4 as i8
	x := 3
	x, y, z := 4, 5, 6
	for i := 0; i < 3; i += 1 {
		_ = i
	}
}
