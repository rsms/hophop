// SLP-13 compound literals in one file:
// - explicit typed: Type{ ... }
// - inferred: { ... } from expected type (init/assign/call/field)
// - nested literals, omitted-field zero fill
// - readonly ref binding from a literal temporary
// - address-of typed literal
// - union single-field initialization

struct Vec2 {
    x i32
    y i32
}

struct Rect {
    pos Vec2
    size Vec2
}

union Number {
    i i32
    f f32
}

fn y_of(v &Vec2) i32 {
    return v.y
}

fn main() {
    var a = Vec2{ x = 1, y = 2 }
    var b Vec2 = { x = 3, y = 4 }
    b = { x = 5, y = 6 }

    // call-site inference for { ... } with a ref parameter
    var d i32 = y_of({ x = 9, y = 10 })

    var p &Vec2 = &Vec2{ x = 11, y = 12 }

    var r = Rect{ pos = { x = 21, y = 22 }, size = { x = 30 } }
    var z Vec2 = {}
    var n = Number{ i = 42 }

    assert a.x + b.y + d + p.y + r.size.y + z.x + n.i == 76
}
