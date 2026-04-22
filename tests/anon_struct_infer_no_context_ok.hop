// Verifies anonymous struct inference no context is accepted.
fn use(v struct {
	a int
	b &str
}) int {
	return v.a
}

fn main() {
	var x     = { a: 1, b: "hello" }
	var y int = use(x)
	assert (y == 1)
}
