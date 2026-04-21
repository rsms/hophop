// Verifies direct null as reference cast is rejected.
fn main() {
	var x &int = null as &int
	_ = x
}
