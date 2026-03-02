import "core/str" { format }

fn main() {
	var out [u8 5]
	var n   uint = format(buf: out, "abcdef")
	assert n == 6 as uint
	assert out[0] == 'a' as u8
	assert out[1] == 'b' as u8
	assert out[2] == 'c' as u8
	assert out[3] == 'd' as u8
	assert out[4] == 0
}
