// Verifies string format nonconst arguments is accepted.
import "str" { format }

fn out_eq(out *[u8], n uint, want &str) bool {
	if n != len(want) as uint {
		return false
	}
	var wantBytes &[u8] = want
	for var i uint = 0; i < n; i += 1 {
		if out[i] != wantBytes[i] {
			return false
		}
	}
	return true
}

fn main() {
	var out [u8 128]
	var a   i64  = 42
	var b   f64  = 3.5
	var s   &str = "hi"
	var n   uint = format(buf: out, "i={i}, f={f}, s={s}", a, b, s)
	assert out_eq(out, n, want: "i=42, f=3.5, s=hi")
	assert out[n] == 0
}
