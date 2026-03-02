// String formatting with runtime output and compile-time validation of literal patterns.
struct Data {
	x int
	y &str
}

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
	var out *str = "                                                                                "
	var n1  u32  = fmt(out, "count: {i}, data: {r}", 123 as i64, Data{ x: 456, y: "hello" })
	assert out_eq(out, n: n1, want: "count: 123, data: { x: 456, y: \"hello\" }")

	var dynFmt &str = "{i}"
	var one    *str = "                "
	var n2     u32  = fmt(one, dynFmt, 7 as i64)
	assert out_eq(out: one, n: n2, want: "7")
}
