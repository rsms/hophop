// Verifies const-eval MIR top-level const local function value is accepted.
fn forty_two() int {
	return 42
}

fn run() int {
	const f = forty_two
	return f()
}

const VALUE = run()

fn main() {
	assert VALUE == 42
}
