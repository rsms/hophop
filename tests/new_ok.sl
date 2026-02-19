fn main() {
    var ma MemAllocator
    var _p *i32 = new(&ma, i32)
    var _q *[i32 4] = new(&ma, i32, 4)
}
