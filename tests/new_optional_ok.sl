fn main() i32 {
    var ma MemAllocator
    var n uint = 4
    var p ?*i32 = new(&ma, i32)
    var q ?*[i32 4] = new(&ma, i32, 4)
    var r ?*[i32] = new(&ma, i32, n)
    if p == null || q == null || r == null {
        return 0
    }
    return 1
}
