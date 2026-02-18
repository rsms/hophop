fn main() i32 {
    var x i32
    var y i32
    var p mut&i32 = &x
    var r mut&i32 = &x
    var q &i32 = r
    r = &y
    *r = 7
    *p = 9

    var a [i32 2]
    var s mut[i32] = a
    s[0] = 1
    var ro [i32] = s

    return ro[0] + *q
}
