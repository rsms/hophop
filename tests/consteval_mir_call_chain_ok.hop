// Verifies const-eval MIR call chain is accepted.
fn add1(x int) int {
	return x + 1
}

fn add2(x int) int {
	return add1(x) + 1
}

const VALUE = add2(5)

fn main() {
	assert VALUE == 7
}
