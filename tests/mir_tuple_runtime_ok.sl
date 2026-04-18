// Verifies MIR runtime behavior for tuple.
fn accepts_pair(p (int, bool)) {
	_ = p
}

fn main() {
	var tagged (int, bool) = (123, true)
	_ = tagged
	accepts_pair((123, true))
}
