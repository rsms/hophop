fn count_to(n i32) i32 {
	var i i32 = 0
	for i < n {
		i += 1
	}
	return i
}

fn main() {
	var a [i32 count_to(2)]
	assert a[0] == 0
}
