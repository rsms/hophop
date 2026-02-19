fn f_a(v int) {}
fn f_b(v int) {}
fn f{f_a, f_b};

fn main() {
    var n int
    f(n)
}
