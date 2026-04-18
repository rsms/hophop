// Verifies anytype pack index runtime expected type mismatch panics.
fn id_i64(x i64) i64 {
	return x
}

fn pick_via_call(j uint, args ...anytype) i64 {
	return id_i64(args[j])
}

fn main() {
	var got i64 = pick_via_call(1, 7 as i64, "x")
}
