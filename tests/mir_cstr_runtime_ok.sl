// Verifies MIR runtime behavior for C string.
fn main() {
	var a &str = "ab"
	var b      = cstr(a)
	assert b[0] == 'a'
	assert b[1] == 'b'

	var s *str = "cd"
	var t      = s.cstr()
	assert t[0] == 'c'
	assert t[1] == 'd'
}
