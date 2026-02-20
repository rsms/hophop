struct Vec2 {
    x i32
    y i32
}

fn by_val(v Vec2) i32 {
    return v.x
}

fn by_ref(v &Vec2) i32 {
    return v.y
}

fn main() {
    var a i32 = by_val({ x = 1, y = 2 })
    var b i32 = by_ref({ x = 3, y = 4 })
    var c i32 = by_ref(Vec2{ x = 5, y = 6 })
    assert(a == 1)
    assert(b == 4)
    assert(c == 6)
}
