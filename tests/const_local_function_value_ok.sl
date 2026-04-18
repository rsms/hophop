// Verifies const local function value is accepted.
fn forty_two() int {
	return 42
}

fn run() int {
	const f = forty_two
	return 42
}

fn main() {
	assert run() == 42
}
