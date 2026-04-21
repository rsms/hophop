// Verifies context convertible allocator is accepted.
struct NeedCtx {
	mem *Allocator
}

fn a() context NeedCtx {
	var p *i32 = new i32
}

fn main() {
	a() context { mem }
}
