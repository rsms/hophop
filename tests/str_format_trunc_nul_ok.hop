// Verifies string format trunc nul is accepted.
import "str" { format }

fn main() {
	var out [u8 5]
	var n   int = format(buf: out, "abcdef")
	assert n == 6
	assert out[0] == 'a'
	assert out[1] == 'b'
	assert out[2] == 'c'
	assert out[3] == 'd'
	assert out[4] == 0
}
