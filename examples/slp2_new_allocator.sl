struct Pair {
    x i32
    y i32
}

fn main() {
    var ma MemAllocator

    var one *Pair = new(&ma, Pair)
    var many *[i32 8] = new(&ma, i32, 8)
    one.x = 20
    one.y = 22
    many[0] = one.x + one.y
    assert many[0] == 42
}
