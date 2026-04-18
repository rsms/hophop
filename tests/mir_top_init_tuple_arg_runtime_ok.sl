// Verifies MIR runtime behavior for top-level initialization tuple argument.
fn accept_pair(p (int, int)) int {
	return 7
}

const VALUE = accept_pair((1, 2))

fn main() {
	assert VALUE == 7
}
