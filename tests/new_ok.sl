fn main() i32 {
    var ma MemAllocator
    var p *i32 = new(&ma, i32)
    var q *[i32 4] = new(&ma, i32, 4)
    return (p == 0 as *i32 || q == 0 as *[i32 4]) as i32
}
