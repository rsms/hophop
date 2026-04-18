// Verifies run evaluator basic.
fn add(a, b i32) i32 {
	return a + b
}

fn main() {
	var x = add(2, b: 3)
	assert x == 5
}
