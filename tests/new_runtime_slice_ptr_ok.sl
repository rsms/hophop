// Verifies runtime behavior for new slice pointer.
fn main() {
	var ma *Allocator = context.mem
	var n  uint       = 4
	var a  *[i32]     = new [i32 n] context ma
	var b  *[i32]     = new [i32 4] context ma
	assert len(a) == 4
	assert len(b) == 4
}
