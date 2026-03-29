fn main() *i32 {
	var ma     = context.mem
	var p *i32 = new i32 with ma
	*p = 7
	assert *p == 7
	return p
}
