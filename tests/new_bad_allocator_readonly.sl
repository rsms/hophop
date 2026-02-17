fn main() i32 {
    var ma MemAllocator
    var ro &MemAllocator = &ma
    var p *i32 = new(ro, i32)
    return p as i32
}
