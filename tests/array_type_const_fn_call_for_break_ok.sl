// Verifies array type const function call for break is accepted.
fn count_until(limit i32) i32 {
	var i     i32 = 0
	var count i32 = 0
	for i = 0; i < limit; i += 1 {
		if i == 3 {
			break
		}
		count += 1
	}
	return count
}

fn main() {
	var a [i32 count_until(10)]
	assert a[0] == 0
}
