import "std/mem"

fn main() i32 {
    var ma Allocator
    var t i32
    var p *i32 = new(&ma, t)
    return p as i32
}
