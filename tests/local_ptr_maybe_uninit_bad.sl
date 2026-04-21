fn f(cond bool, y *int) int {
	var x *int
	if cond {
		x = y
	}
	return *x
}
