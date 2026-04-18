// Verifies const block MIR break continue is accepted.
fn main() {
	const {
		var i   int = 0
		var sum int = 0
		for i = 0; i < 6; i += 1 {
			if i == 1 {
				continue
			}
			if i == 4 {
				break
			}
			sum += i
		}
		assert sum == 5
	}
}
