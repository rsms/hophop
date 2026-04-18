// Verifies runtime behavior for string index inference u8.
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
	var a u8   = first_ro("xyz")
	var b u8   = first_rw(m)
	assert a == 'x' as u8
	assert b == 'a' as u8
}
