fn first_ro(text &str) u8 {
	var b = text
	return b[0]
}

fn first_rw(text *str) u8 {
	var b = text
	return b[0]
}

fn main() {
	var m *str = "abc"
	const {
		var c u8 = "xyz"[1]
		assert c == 'y'
	}
	var a u8 = first_ro("xyz")
	var b u8 = first_rw(m)
	assert a == 'x'
	assert b == 'a'
}
