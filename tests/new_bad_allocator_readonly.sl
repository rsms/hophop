import "std/mem"

fn main() i32 {
    var ma mem.Allocator
    var ro &mem.Allocator = &ma
    var p *i32 = ro.new(i32)
    return p as i32
}
