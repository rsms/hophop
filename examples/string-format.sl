import "core/str" { format }

fn build_showcase_format() &str {
	return "user={s}, score={i}, ratio={f}, braces={{ok}}"
}

const SHOWCASE_FORMAT &str = build_showcase_format()

fn out_eq(out *[u8], n uint, want &str) bool {
	if n != len(want) {
		return false
	}
	var want_bytes &[u8] = want
	for var i uint = 0; i < n; i += 1 {
		if out[i] != want_bytes[i] {
			return false
		}
	}
	return true
}

fn main() {
	// Placeholder support: {s}, {i}, {f}. Escape braces with {{ and }}.
	var out [u8 128]
	var n   uint = format(buf: out, SHOWCASE_FORMAT, "Ada", 42, 0.625)
	assert out_eq(out, n, want: "user=Ada, score=42, ratio=0.625, braces={ok}")
	assert out[n] == 0

	// Truncation contract:
	// - return value is full logical payload length
	// - if len(buf) > 0, write at most len(buf)-1 payload bytes and trailing NUL
	var tiny [u8 8]
	var tn   uint = format(buf: tiny, "abcdefghi")
	assert tn == 9 as uint
	assert tiny[0] == 'a' as u8
	assert tiny[1] == 'b' as u8
	assert tiny[2] == 'c' as u8
	assert tiny[3] == 'd' as u8
	assert tiny[4] == 'e' as u8
	assert tiny[5] == 'f' as u8
	assert tiny[6] == 'g' as u8
	assert tiny[7] == 0

	// Zero-capacity buffer: writes nothing, still returns full logical length.
	var zero [u8 0]
	var zn   uint = format(buf: zero, "abcdef")
	assert zn == 6 as uint
}
