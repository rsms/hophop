// SLP-29: for-in loops (value, ref, ptr, key+value, and discard)
fn main() {
	var a [i32 4]
	a[0] = 1
	a[1] = 2
	a[2] = 3
	a[3] = 4

	// Pointer capture over a mutable slice updates elements in place.
	var s *[i32] = a[:]
	for *item in s {
		*item *= 2
	}

	var ro &[i32] = s

	// Value capture (copy each element).
	var sum i32
	for item in ro {
		sum += item
	}

	// Ref capture (bind item as &i32).
	var sum_ref i32
	for &item in ro {
		sum_ref += *item
	}

	// Key + value capture.
	var weighted i32
	for i, item in ro {
		weighted += item * i as i32
	}

	// Key + discarded value.
	var index_sum uint
	for i, _ in ro {
		index_sum += i
	}

	// Value discard.
	var count uint
	for _ in ro {
		count += 1
	}

	assert sum == 20
	assert sum_ref == 20
	assert weighted == 40
	assert index_sum == 6
	assert count == 4
}
