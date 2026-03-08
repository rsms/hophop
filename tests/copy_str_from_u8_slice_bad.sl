fn main() {
	var dst *str = "abc"
	var raw [u8 3]
	_ = copy(dst, raw[:])
}
