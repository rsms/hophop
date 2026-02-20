struct Point {
    x i32
    y i32
}

fn main() {
    var p = Point{ x = 10, y = 20 }
    var q Point = { x = 30, y = 40 }
    _ = p
    _ = q
}
