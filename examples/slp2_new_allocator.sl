struct Pair {
    x i32
    y i32
}

fn main() i32 {
    var ma MemAllocator

    var one *Pair = new(&ma, Pair)
    var many *[i32 8] = new(&ma, i32, 8)

    if one == 0 as *Pair || many == 0 as *[i32 8] {
        return 1
    }

    one.x = 20
    one.y = 22
    many[0] = one.x + one.y
    return many[0]
}
