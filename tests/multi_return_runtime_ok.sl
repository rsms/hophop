import "platform"

var calls uint = 0

fn next_pair() (uint, uint) {
	calls = calls + 1
	return calls, calls + 10
}

fn main() {
	var a, b = next_pair()
	if calls != 1 || a != 1 || b != 11 {
		platform.exit(1)
	}

	a, b = next_pair()
	if calls != 2 || a != 2 || b != 12 {
		platform.exit(2)
	}

	platform.exit(0)
}
