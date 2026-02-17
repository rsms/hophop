fn main() i32 {
    var ma MemAllocator
    var t i32
    var p *i32 = new(&ma, t)
    return p as i32
}
