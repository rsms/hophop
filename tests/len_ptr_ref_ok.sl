fn main() {
	var ma         = context.mem
	var a [i32 4]
	var p *[i32 4] = new [i32 4] with ma
	var r &[i32 4] = p
	var m *[i32 4] = &a
	var s &[i32]   = a[:]
	assert (len(a) + len(p) + len(r) + len(m) + len(s)) as i32 == 20
}
