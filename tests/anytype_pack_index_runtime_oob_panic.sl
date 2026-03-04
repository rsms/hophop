fn pick_i64(j uint, args ...anytype) i64 {
	return args[j] as i64
}

fn main() {
	var x i64 = pick_i64(2, 1 as i64)
}
