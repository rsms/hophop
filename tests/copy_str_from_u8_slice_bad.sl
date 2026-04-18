// Verifies copy string from u8 slice is rejected.
fn main() {
	var dst *str = "abc"
	var raw [u8 3]
	_ = copy(dst, raw[:])
}
