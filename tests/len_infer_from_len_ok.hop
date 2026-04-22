// Verifies len inference from len is accepted.
fn payload_cap(buf *[u8]) int {
	var cap   = len(buf)
	var p int = 0
	if cap > 0 {
		p = cap - 1
	}
	return p
}

fn main() {
	var out [u8 4]
	var n = payload_cap(out)
	assert n == 3
}
