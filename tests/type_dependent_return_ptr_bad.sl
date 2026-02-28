fn allocate(T type) *T

fn main() {
	var p *u8 = allocate(i64)
	_ = p
}
