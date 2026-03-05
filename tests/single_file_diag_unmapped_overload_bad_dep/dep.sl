pub fn g(v anytype) uint {
	return bad(v)
}

fn bad(x u8) uint {
	return x as uint
}
