fn first(v &[i32]) i32 {
	return v[0]
}

fn main() {
	var values [i32 3]
	values[0] = 7
	values[1] = 9
	values[2] = 11
	assert first(values[:]) == 7
}
