// Verifies SLP 29 for in value pointer immutable is rejected.
fn main() {
	var a  [i32 2]
	var ro &[i32] = a[:]
	for *v in ro {
		*v += 1
	}
}
