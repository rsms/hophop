import "platform"

fn alloc() *u8 context platform.Context {
    return new(u8)
}

fn main() {
    var p *u8 = alloc()
    alloc()
}
