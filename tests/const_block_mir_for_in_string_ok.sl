// Verifies const block MIR for in string is accepted.
fn next() int {
	const {
		var sum int = 0
		for i, ch in "ABC" {
			sum += i as int
			sum += ch as int
		}
		assert sum == ('A' as int + 'B' as int + 'C' as int + 0 + 1 + 2)
	}
	return 1
}

fn main() {
	assert next() == 1
}
