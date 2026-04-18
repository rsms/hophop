// Verifies formatter output for rewrite literal casts.
var x u32 = 3

fn f() u32 {
	return 3
}

fn g(v i16) {
	var y i16 = 4 * 9
	_ = v + 4
	var z u32 = 3
}
