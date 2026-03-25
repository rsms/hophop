struct Box {
	p *i32
}

fn write_box(b *Box) {
	*b.p =  9
	*b.p += 5
}

fn main() {
	var value i32 = 1
	var box       = Box{}
	box.p = &value
	write_box(&box)
	assert value == 14
}
