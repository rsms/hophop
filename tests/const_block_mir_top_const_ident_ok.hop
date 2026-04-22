// Verifies const block MIR top-level const identifier is accepted.
const BASE = 40

fn run() i32 {
	return BASE + 2
}

fn compute() i32 {
	const {
		assert run() == 42
	}
	return 42
}

fn main() {
	assert compute() == 42
}
