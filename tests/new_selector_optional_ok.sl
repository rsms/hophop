// Verifies new selector optional is accepted.
fn main() {
	var ma          = context.mem
	var n uint      = 4
	var p ?*i32     = new i32 with ma
	var q ?*[i32 4] = new [i32 4] with ma
	var r ?*[i32]   = new [i32 n] with ma
	assert p != null
	assert q != null
	assert r != null
}
