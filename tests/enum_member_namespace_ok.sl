enum Mode i32 {
    A = 0
    B = 1
}

enum Color i32 {
    A = 0
    B = 1
}

fn is_mode_a(m Mode) bool {
    return m == Mode.A
}

fn is_color_a(c Color) bool {
    return c == Color.A
}

fn main() {
    var m Mode = Mode.A
    var c Color = Color.A
    assert is_mode_a(m)
    assert is_color_a(c)
}
