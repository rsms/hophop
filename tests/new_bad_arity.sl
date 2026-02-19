import "std/mem" as mem

fn main() i32 {
    var ma mem.Allocator
    var p *i32 = new(&ma)
    return p as i32
}
