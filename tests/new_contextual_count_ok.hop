// Verifies new contextual count is accepted.
fn allocs(n uint) {
	var a *[i32 4] = new [i32 4]
	var b *[i32]   = new [i32 n]

	a[0] = 11
	b[0] = a[0]
	assert len(a) == 4
	assert len(b) == 4
	assert b[0] == 11
}

fn main() {
	allocs(4)
}
