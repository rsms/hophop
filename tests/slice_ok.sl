fn main() {
    var a [i32 4]
    var i i32 = 1
    a[0] = 1
    a[1] = 2
    a[2] = 3
    a[3] = 4

    var s *[i32] = a[1:3]
    s[0] = 9

    var ro &[i32] = a[:]
    var tail &[i32] = s[1:]
    var head &[i32] = s[:1]
    var dyn &[i32] = a[i:]

    assert (len(s) + len(ro) + len(tail) + len(head) + len(dyn)) as i32 == 11
}
