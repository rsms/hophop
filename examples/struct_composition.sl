// Example: struct composition via embedded fields and promoted member access.

struct A {
    x int
}

struct B {
    A
    y int
}

struct C {
    B
    z int
}

fn main() {
    var b B
    var c C

    c.x = 1
    c.y = 2
    c.z = 3

    b = c

    assert b.x == 1
    assert b.y == 2
}
