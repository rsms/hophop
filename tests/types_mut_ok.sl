fn main() {
    var x i32
    var y i32
    var p *i32 = &x
    var r *i32 = &x
    var q &i32 = r
    r = &y
    *r = 7
    *p = 9

    var a [i32 2]
    var s *[i32] = a
    s[0] = 1
    var ro &[i32] = s

    assert ro[0] + *q == 10
}
