struct Vec2 {
    x i32
    y i32
}

struct Vec3 {
    x i32
    y i32
    z i32
}

fn measure(v Vec2) i32 {
    return v.x + v.y
}

fn measure(v Vec3) i32 {
    return v.x + v.y + v.z
}

fn main() {
    var n i32 = measure({ x = 1, y = 2 })
    assert(n == 0)
}
