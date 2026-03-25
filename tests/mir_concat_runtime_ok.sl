fn main() {
	var a &str = "ab"
	var b &str = "cd"
	var s *str = concat(a, b)
	assert len(s) == 4
	assert s[0] == 'a'
	assert s[1] == 'b'
	assert s[2] == 'c'
	assert s[3] == 'd'
}
