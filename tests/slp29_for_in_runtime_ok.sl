fn main() {
	var a [i32 3]
	a[0] = 1
	a[1] = 2
	a[2] = 3

	var s *[i32] = a[:]
	for &item in s {
		*item += 10
	}

	var ro  &[i32] = s
	var sum i32
	for &item in ro {
		sum += *item
	}

	var weighted i32
	for i, item in ro {
		weighted += item * i as i32
	}

	var n uint
	for _ in ro {
		n += 1
	}
	assert sum == 36
	assert weighted == 38
	assert n == 3
}
