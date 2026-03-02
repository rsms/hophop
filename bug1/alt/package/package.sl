pub fn simple(x i64, y i64) i64 {
	return x + y
}

pub fn shaped(buf *[u8], const format &str, args ...anytype) uint {
	_ = buf
	_ = format
	_ = args
	return 0
}
