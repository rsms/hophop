fn set_second(v *[i32]) {
	v[1] = 42
}

fn main() {
	var values [i32 3]
	set_second(values[:])
	assert values[1] == 42
}
