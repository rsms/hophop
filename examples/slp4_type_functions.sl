struct A {
    x int
}

struct B {
    A
    y int
}

struct Stats {
    len u32
}

fn pick_a(v A) int {
    return v.x
}

fn pick_b(v B) int {
    return v.y
}

fn pick{pick_a, pick_b};

fn main() {
    var a A
    var b B
    var s Stats
    var msg str = "hi"
    var ma mut&MemAllocator = 0 as mut&MemAllocator

    a.x = 1
    b.x = 2
    b.y = 3

    assert pick(a) == 1
    assert b.pick() == 3

    s.len = 7
    assert s.len == 7

    assert msg.len() == len(msg)

    var p ?*i32 = ma.new(i32)
    assert p == null
}
