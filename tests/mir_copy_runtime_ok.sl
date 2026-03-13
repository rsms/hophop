fn main() {
	var dst *str = "zz"
	assert copy(dst, "ab") == 2
	assert dst[0] == 'a' as u8
	assert dst[1] == 'b' as u8
}
