// Verifies string concatenation del is accepted.
fn caller() {
	var r &str = "abc"
	var s *str = concat(r, r)
	assert len(s) == 6
	del s

	var arr *[u8 4] = new [u8 4]
	del arr
}

fn main() {
	caller()
}
