fn noop() {}

fn example(a, _, c int) int {
	noop()
	noop()
	return a + c
}
