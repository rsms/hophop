fn choose(v ?i32) i32 {
	if v {
		return v
	}
	return 9
}

fn main() {
	var a ?i32 = 0
	if a {
		assert a == 0
	} else {
		assert false
	}

	var b ?i32 = null
	if b {
		assert false
	} else {
		assert b == null
	}

	assert choose(a) == 0
	assert choose(b) == 9
}
