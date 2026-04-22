// Verifies new contextual count optional is accepted.
fn allocs(n uint) {
	var a ?*[i32 4] = new [i32 4]
	var b ?*[i32]   = new [i32 n]
	assert a != null
	assert b != null
}

fn main() {
	allocs(4)
}
