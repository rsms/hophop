fn sum(xs [i32]) i32 {
    var n u32 = len(xs)
    return xs[0] + n as i32
}

fn view(xs mut[i32]) [i32] {
    return xs
}

fn main() i32 {
    var a [i32 3]
    a[0] = 5
    var s [i32] = view(a[:])
    return sum(a) + sum(s)
}
