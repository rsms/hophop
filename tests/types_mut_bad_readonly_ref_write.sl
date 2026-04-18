// Verifies types mutable bad read-only reference write.
fn main() i32 {
	var x  i32
	var ro &i32 = &x
	*ro = 1
	return 0
}
