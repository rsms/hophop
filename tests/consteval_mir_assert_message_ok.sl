fn run() int {
	assert 1 == 1, "ok %d", 1
	return 7
}

const VALUE = run()

fn main() {
	assert VALUE == 7
}
