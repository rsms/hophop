struct Size {
    w i32
    h i32
}

struct Window {
    size Size
    title str
}

fn main() {
    var w Window = {
        size = { w = 800, h = 600 },
        title = "sl",
    }
    assert(w.size.w == 800)
    assert(w.size.h == 600)
}
