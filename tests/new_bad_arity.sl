fn main() i32 {
    var ma MemAllocator
    var p *i32 = new(&ma)
    return p as i32
}
