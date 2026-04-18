// Verifies const block MIR switch is accepted.
fn next() int {
	const {
		var y int = 0
		switch 2 {
			case 2  { y = 7 }
			default { y = 3 }
		}
		assert y == 7
	}
	return 1
}

fn main() {
	assert next() == 1
}
