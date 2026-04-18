// Verifies const block MIR local function value is accepted.
fn forty_two() int {
	return 42
}

fn main() {
	const {
		const f = forty_two
		assert f() == 42
	}
}
