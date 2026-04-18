// Verifies const-eval MIR tuple return is accepted.
fn pair(x, y int) (int, int) {
	return x + 1, y + 2
}

fn sum_pair(x, y int) int {
	var a, b = pair(x, y)
	return a + b
}

const VALUE = sum_pair(10, y: 20)

fn main() {
	assert VALUE == 33
}
