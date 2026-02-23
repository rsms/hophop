
fn main() i32 {
    var ma Allocator
    var ro &Allocator = &ma
    var p *i32 = ro.new(i32)
    return p as i32
}
