fn main() i32 {
	var x i32  = 1
	var p *i32 = &x

	assert *p == 1
	*p = 7
	assert x == 7

	return *p
}
