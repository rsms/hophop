fn concat(a, b &str) *str context { mem *Allocator } {
	var lenA uint = len(a)
	var lenB uint = len(b)
	var out  *str = new str{ len: lenA + lenB }

	copy(out, a)
	copy(out[lenA:], b)
	out.ptr[len(out)] = 0
	return out
}
