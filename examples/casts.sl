// explicit type conversion using `as` between numeric types

fn main() {
    var x i32 = 7
    var y f64 = x as f64

    if y > 0.0 {
        assert x == 7
    } else {
        assert 0 == 1
    }
}
