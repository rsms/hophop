// Verifies runtime behavior for anytype pack index.
fn pick_i64(j uint, args ...anytype) i64 {
	return args[j] as i64
}

fn pick_str(j uint, args ...anytype) &str {
	return args[j] as &str
}

fn main() {
	assert pick_i64(0, 42 as i64, -7 as i64) == 42 as i64
	assert pick_i64(1, 42 as i64, -7 as i64) == -7 as i64
	assert pick_str(1, "a", "b", "c") == "b"
}
