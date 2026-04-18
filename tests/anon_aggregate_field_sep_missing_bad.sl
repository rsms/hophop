// Verifies anonymous aggregate field sep missing is rejected.
fn main() {
    var x struct { a int b bool } = { a: 1, b: true }
    assert x.b
}
