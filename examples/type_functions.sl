fn main() {
    var msg str = "hi"
    var ma mut&MemAllocator = 0 as mut&MemAllocator

    assert msg.len() == len(msg)

    var p ?*i32 = ma.new(i32)
    var q ?*[i32 4] = ma.new(i32, 4)

    assert p == null
    assert q == null
}
