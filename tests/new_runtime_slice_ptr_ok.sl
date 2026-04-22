// Verifies runtime behavior for new slice pointer.
fn main() {
	var n uint   = 4
	var a *[i32] = new [i32 n]
	var b *[i32] = new [i32 4]
	assert len(a) == 4
	assert len(b) == 4
}
