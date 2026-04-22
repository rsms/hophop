// Verifies runtime behavior for anytype pack index anytype call.
fn score(v anytype) i64 {
	const t = typeof(v)
	if t == i64 {
		return v as i64
	}
	if t == f64 {
		return v as f64 as i64
	}
	return -1 as i64
}

fn pick(j uint, args ...anytype) i64 {
	return score(v: args[j])
}

fn main() {
	assert pick(0, 7 as i64, 3.5 as f64) == 7 as i64
	assert pick(1, 7 as i64, 3.5 as f64) == 3 as i64
}
