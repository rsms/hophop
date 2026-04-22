// Verifies const-eval MIR break continue is accepted.
fn run() int {
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
	return sum
}

const VALUE = run()

fn main() {
	assert VALUE == 5
}
