// Verifies types mutable bad reference assign value.
fn main() i32 {
	var x i32
	var r *i32 = &x
	r = 1
	return 0
}
