// Verifies const-eval MIR local aggregate is accepted.
struct Pair {
	left  int
	right int
}

fn run() int {
	var p Pair
	p.left = 7
	p.right = 3
	return p.left + p.right
}

const VALUE = run()

fn main() {
	assert VALUE == 10
}
