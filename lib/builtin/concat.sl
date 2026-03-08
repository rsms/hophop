fn concat(a, b &str) *str context { mem *Allocator } {
	var lenA   uint  = len(a)
	var lenB   uint  = len(b)
	var outLen uint  = lenA + lenB
	var out    *str  = new str{ len: outLen }
	var bytes  *[u8] = out

	copy(out, a)
	copy(bytes[lenA:], b)
	out.ptr[outLen] = 0
	return out
}
