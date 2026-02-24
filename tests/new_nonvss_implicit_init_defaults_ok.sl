struct S {
    a i32 = 7
    b i32 = a + 2
}

fn main() {
    var ma = context.mem

    var p *S = new S with ma
    var q *S = new S{} with ma

    assert p.a == 7
    assert p.b == 9
    assert q.a == 7
    assert q.b == 9
}
