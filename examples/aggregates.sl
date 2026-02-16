pub struct Pair {
    x i32
    y i32
}

pub union Number {
    i i32
    f f32
}

pub enum Mode i32 {
    Mode_A = 0
    Mode_B = 1
}

pub fn main() i32;

fn main() i32 {
    var p Pair
    p.x = 4
    p.y = 5

    var n Number
    n.i = p.x + p.y
    return n.i
}
