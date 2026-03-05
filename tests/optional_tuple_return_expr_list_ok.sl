fn pair_or_none(flag bool) ?(i32, i32) {
	if !flag {
		return null
	}
	return 4, 9
}

fn main() {
	var some = pair_or_none(true)
	if some {
		assert true
	} else {
		assert false
	}

	var none = pair_or_none(false)
	if none {
		assert false
	}
}
