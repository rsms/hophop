// Verifies array type const function call for infinite return is accepted.
fn count_to(n i32) i32 {
	var i i32 = 0
	for {
		if i >= n {
			return i
		}
		i += 1
	}
}

fn main() {
	var a [i32 count_to(2)]
	assert a[0] == 0
}
