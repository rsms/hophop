fn main() i32 {
    var a [i32 4]
    var s &[i32] = a[:5]
    return len(s) as i32
}
