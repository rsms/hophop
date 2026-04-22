// Verifies formatter output for rewrite comparison literal casts.
fn current_i32() i32 {
	return 1
}

fn example(z i64, x i32) {
	assert z == 0
	assert 0 == z
	assert z != 1
	assert z < 2
	assert 3 <= z
	assert z > 4
	assert 5 >= z
	assert x == 0
	assert 0 == x
	assert current_i32() == 0
}
