// Verifies MIR runtime behavior for free.
fn main() {
	var s *str = concat("a", "b")
	assert len(s) == 2
	del s
}
