// Verifies direct null as pointer cast is rejected.
fn main() {
	var x *int = null as *int
	_ = x
}
