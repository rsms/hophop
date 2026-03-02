fn out_eq(out *str, n u32, want &str) bool {
	if n != len(want) {
		return false
	}
	var outBytes  *[u8] = out
	var wantBytes &[u8] = want
	for var i u32 = 0; i < n; i += 1 {
		if outBytes[i] != wantBytes[i] {
			return false
		}
	}
	return true
}

fn main() {
	var a  *str = "                                                                                "
	var na u32  = fmt(a, "{{{i}}}", 42 as i64)
	assert out_eq(out: a, n: na, want: "{42}")

	var b  *str = "                                                                                 "
	var nb u32  = fmt(b, "plain {{ and }} braces")
	assert out_eq(out: b, n: nb, want: "plain { and } braces")
}
