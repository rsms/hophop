// Verifies struct field sep missing is rejected.
struct S {
    x int y bool
}

fn main() {
    var s S = { x: 1, y: true }
    assert s.y
}
