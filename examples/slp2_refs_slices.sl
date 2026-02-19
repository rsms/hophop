// Example: references and slices, including mutability and implicit view conversions.

fn setfirst(xs mut&[i32 4], v i32) {
    xs[0] = v
}

fn rosum(xs &[i32 4]) i32 {
    return xs[0] + len(xs) as i32
}

fn head(xs mut[i32]) [i32] {
    return xs[:1]
}

fn main() {
    var a [i32 4]

    var mr mut&i32 = &a[2]
    *mr = 7
    var rr &i32 = mr

    var arf mut&[i32 4] = &a
    setfirst(arf, 9)

    var rof &[i32 4] = arf
    var all mut[i32] = a[:]
    var tail [i32] = all[1:]
    var first [i32] = head(all)

    assert rosum(rof) + *rr + len(tail) as i32 + len(first) as i32 == 24
}
