fn allocate(T type) *T

fn main() {
	var p *i64 = allocate(i64)
	var q *u8  = allocate(u8)
	_ = p
	_ = q
}
