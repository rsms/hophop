fn type_is_i64(j uint, args ...anytype) bool {
	return typeof(args[j]) == typeof(0 as i64)
}

fn main() {
	assert type_is_i64(0, 1 as i64)
	assert !type_is_i64(1, 1 as i64, "x")
}
