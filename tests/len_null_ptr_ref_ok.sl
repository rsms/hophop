fn main() {
	var p *[i32 4] = null as *[i32 4]
	var r &[i32 4] = p
	assert len(p) + len(r) == 0
}
