// Verifies const-eval MIR for in string is accepted.
fn sum_text() int {
	var sum int = 0
	for i, ch in "ABC" {
		sum += i as int
		sum += ch as int
	}
	return sum
}

const SUM int = sum_text()

fn main() {
	assert SUM == ('A' as int + 'B' as int + 'C' as int + 0 + 1 + 2)
}
