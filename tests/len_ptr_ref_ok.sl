fn main() i32 {
    var a [i32 4]
    var p *[i32 4] = &a
    var r &[i32 4] = p
    var m mut&[i32 4] = &a
    var s [i32] = a[:]
    var ps *[i32] = &s
    return (len(a) + len(p) + len(r) + len(m) + len(s) + len(ps)) as i32
}
