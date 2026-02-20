import "std/mem"

fn main() i32 {
    var ma mem.Allocator
    var p *i32 = new(&ma, i32, 1, 2)
    return p as i32
}
