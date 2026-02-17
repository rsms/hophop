fn main() i32 {
    var p *[i32 4] = 0 as *[i32 4]
    var r &[i32 4] = p
    return (len(p) + len(r)) as i32
}
