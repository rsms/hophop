// Verifies const block MIR call chain is accepted.
fn double(const x int) int {
	return x * 2
}

fn compute() int {
	const {
		const value int = double(21)
		assert value == 42
	}
	return 42
}

fn main() {
	assert compute() == 42
}
