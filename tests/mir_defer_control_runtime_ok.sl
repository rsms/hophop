fn main() {
	var acc i32
	var i   i32

	for i < 3 {
		{
			defer acc += 100
			if i == 0 {
				i += 1
				continue
			}
			if i == 1 {
				break
			}
			acc += 1
		}
		i += 1
	}

	assert acc == 200
	assert i == 1
}
