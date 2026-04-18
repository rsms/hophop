// Verifies const-eval MIR simple function is accepted.
fn classify(x int) int {
	if x > 1 {
		return 7
	}
	return 3
}

const VALUE = classify(2)

fn main() {
	assert VALUE == 7
}
