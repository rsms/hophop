// Verifies new optional is accepted.
fn main() {
	var n uint      = 4
	var p ?*i32     = new i32
	var q ?*[i32 4] = new [i32 4]
	var r ?*[i32]   = new [i32 n]
	assert p != null
	assert q != null
	assert r != null
}
