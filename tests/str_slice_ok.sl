// Verifies string slice is accepted.
fn main() {
	var s *str = concat("ab", "cde")
	var t *str = s[2:]
	assert len(t) == 3
	assert t[0] == 'c' as u8
	assert copy(s[2:], "XY") == 2
	assert s[2] == 'X' as u8
	assert s[3] == 'Y' as u8
	free(s)
}
