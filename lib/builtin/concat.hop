fn concat(a, b &str) *str {
	var lenA int  = len(a)
	var lenB int  = len(b)
	var out  *str = new str{ len: lenA + lenB }

	copy(out, a)
	copy(out[lenA:], b)
	out.ptr[len(out)] = 0
	return out
}
