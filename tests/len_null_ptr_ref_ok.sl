fn main() {
	var p *[i32 4] = 0
	var r &[i32 4] = p
	assert len(p) + len(r) == 0
}
