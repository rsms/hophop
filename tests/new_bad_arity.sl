
fn main() i32 {
    var ma Allocator
    var p *i32 = new(&ma, i32, 1, 2)
    return p as i32
}
