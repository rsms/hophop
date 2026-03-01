import "std/testing"

fn main() {
	var ma *Allocator = null as *Allocator
	var n  uint       = 4
	var p  ?*i32      = new i32 with ma
	var q  ?*[i32 4]  = new [i32 4] with ma
	var r  ?*[i32]    = new [i32 n] with ma
	assert p == null
	assert q == null
	assert r == null
}
