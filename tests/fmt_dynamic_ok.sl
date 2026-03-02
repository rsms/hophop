struct Data {
	x int
	y &str
}

fn out_eq(out *str, n uint, want &str) bool {
	if n != len(want) {
		return false
	}
	var outBytes  *[u8] = out
	var wantBytes &[u8] = want
	for var i uint = 0; i < n; i += 1 {
		if outBytes[i] != wantBytes[i] {
			return false
		}
	}
	return true
}

fn pick(flag bool) &str {
	if flag {
		return "value={i}, rec={r}"
	}
	return "value={r}"
}

fn main() {
	var out *str = "                                                                                "
	var n   uint = fmt(out, pick(true), 7 as i64, Data{ x: 9, y: "z" })
	assert out_eq(out, n, want: "value=7, rec={ x: 9, y: \"z\" }")
}
