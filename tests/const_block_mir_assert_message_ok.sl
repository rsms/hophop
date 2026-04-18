// Verifies const block MIR assert message is accepted.
fn main() {
	const {
		assert 1 == 1, "ok %d", 1
	}
}
