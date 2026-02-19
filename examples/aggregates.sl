// struct, union, and enum declarations with basic usage

pub struct Pair {
    x i32
    y i32
}

pub union Number {
    i i32
    f f32
}

pub enum Mode i32 {
    A = 0
    B = 1
}

fn is_mode_a(m Mode) bool {
    return m == Mode.A
}

fn main() {
    var p Pair
    p.x = 4
    p.y = 5

    var n Number
    n.i = p.x + p.y
    assert n.i == 9

    var mode = Mode.A
    assert is_mode_a(mode)
}
