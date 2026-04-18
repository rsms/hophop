// Verifies const block MIR overload call is accepted.
struct Cat {}

struct Dog {}

fn pick(v Cat) int {
	return 1
}

fn pick(v Dog) int {
	return 2
}

fn main() {
	const {
		var dog Dog
		assert pick(dog) == 2
	}
}
