fn ret_scalar(x int) int {
	return x
}

fn ret_expr(a, b int) int {
	return a + b
}

fn ret_tuple(a, b, c int) (int, int, int) {
	return a, b, c
}

fn ret_nested_tuple(a, b int) (int, int) {
	return a, b
}

fn ret_void(x int) {
	if x > 0 {
		return
	}
}
