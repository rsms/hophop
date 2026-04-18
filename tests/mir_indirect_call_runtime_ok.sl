// Verifies MIR runtime behavior for indirect call.
fn twice(x int) int {
	return x * 2
}

fn main() {
	var f = twice
	assert f(21) == 42
}
