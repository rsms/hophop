// Verifies comparison hook impure global is rejected.
const G int = 1

fn __equal(a, b int) bool {
	return G == (a - b)
}
