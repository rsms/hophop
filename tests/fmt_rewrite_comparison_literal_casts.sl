// Verifies formatter output for rewrite comparison literal casts.
fn current_i32() i32 {
return 1
}

fn example(z i64, x i32) {
assert z==(0 as i64)
assert (0 as i64)==z
assert z!=(1 as i64)
assert z<(2 as i64)
assert (3 as i64)<=z
assert z>(4 as i64)
assert (5 as i64)>=z
assert x==(0 as i32)
assert (0 as i32)==x
assert current_i32()==(0 as i32)
}
