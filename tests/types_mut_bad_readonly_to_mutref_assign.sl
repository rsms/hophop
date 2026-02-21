fn main() i32 {
    var x i32
    var y i32
    var mr *i32 = &x
    var ro &i32 = &y
    mr = ro
    return 0
}
