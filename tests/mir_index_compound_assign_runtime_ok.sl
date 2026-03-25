fn bump_second(v *[i32]) {
	v[1] += 5
}

fn main() {
	var values [i32 3]
	values[1] = 4
	bump_second(values[:])
	assert values[1] == 9
}
