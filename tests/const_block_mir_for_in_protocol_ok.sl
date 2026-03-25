fn __iterator(start i32) i32 {
	return start
}

fn next_value(it *i32) ?i32 {
	var cur = *it
	if cur > 3 {
		return null
	}
	*it = cur + 1
	return cur
}

fn __iterator(_ bool) i32 {
	return 0
}

fn next_key(it *i32) ?i32 {
	var cur = *it
	if cur == 0 {
		*it = 1
		return 1
	}
	if cur == 1 {
		*it = 2
		return 2
	}
	return null
}

fn __iterator(source u8) i32 {
	return source as i32
}

fn next_key_and_value(it *i32) ?(&str, *i32) {
	var cur = *it
	if cur == 0 {
		return null
	}
	*it = 0
	return "v", it
}

fn main() {
	const {
		var counterSum i32
		for value in (1 as i32) {
			counterSum += value
		}
		assert counterSum == 6

		var keySum i32
		for key, _ in true {
			keySum += key
		}
		assert keySum == 3

		var pairValueSum i32
		for value in (7 as u8) {
			pairValueSum += value
		}
		assert pairValueSum == 7

		var pairRefSum i32
		for key, &value in (5 as u8) {
			if len(key) == 1 {
				pairRefSum += *value
			}
		}
		assert pairRefSum == 5
	}
}
