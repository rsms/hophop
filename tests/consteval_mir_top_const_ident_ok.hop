// Verifies const-eval MIR top-level const identifier is accepted.
const BASE = 40

fn run() i32 {
	return BASE + 2
}

const VALUE = run()

fn main() {
	assert VALUE == 42
}
