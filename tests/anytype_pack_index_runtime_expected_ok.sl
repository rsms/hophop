fn id_i64(x i64) i64 {
	return x
}

fn pick_via_call(j uint, args ...anytype) i64 {
	return id_i64(args[j])
}

fn pick_via_assign(j uint, args ...anytype) i64 {
	var x i64 = args[j]
	return x
}

fn main() {
	assert pick_via_call(0, 7 as i64, 9 as i64) == 7 as i64
	assert pick_via_assign(1, 7 as i64, 9 as i64) == 9 as i64
}
