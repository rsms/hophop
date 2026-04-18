// Verifies string slice mutable is rejected.
fn main() {
	var r  &str  = "abc"
	var rw *[u8] = r
}
