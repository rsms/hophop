// Verifies const-eval MIR top-level const local function value argument is accepted.
fn add1(x int) int {
	return x + 1
}

fn run() int {
	const f = add1
	return f(41)
}

const VALUE = run()

fn main() {
	assert VALUE == 42
}
