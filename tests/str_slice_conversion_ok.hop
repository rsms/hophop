// Verifies string slice conversion is accepted.
fn main() {
	var r   &str  = "abc"
	var m   *str  = "abc"
	var ro1 &[u8] = r
	var ro2 &[u8] = m
	var rw  *[u8] = m
	assert len(ro1) == 3
	assert len(ro2) == 3
	assert len(rw) == 3
}
