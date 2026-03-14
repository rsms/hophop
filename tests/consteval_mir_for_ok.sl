fn run() int {
	var i   int = 0
	var sum int = 0
	for i = 0; i < 4; i += 1 {
		sum += i
	}
	return sum
}

const VALUE = run()

fn main() {
	assert VALUE == 6
}
