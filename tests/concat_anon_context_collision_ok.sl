// Verifies concatenation anonymous context collision is accepted.
fn choose(v ?i32) i32 {
	if v {
		return v
	}
	return 9
}

fn main() {
	var a    &str = "ab"
	var b    &str = "cd"
	var s    *str = concat(a, b)
	var none ?i32 = null

	assert choose(0) == 0
	assert choose(none) == 9
	assert s[0] == 'a'
	assert s[1] == 'b'
	assert s[2] == 'c'
	assert s[3] == 'd'
}
