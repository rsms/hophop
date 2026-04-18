// Verifies anonymous aggregate field sep comma is rejected.
fn main() {
    var x struct { a int, b bool } = { a: 1, b: true }
    _ = x
}
