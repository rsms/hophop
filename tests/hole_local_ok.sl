fn noop() {}

fn example(a int, _ int, c int) int {
	noop()
	noop()
	return a + c
}
