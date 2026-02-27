fn code(x i32) i32 {
    if x == 0 {
        return 11
    } else if x == 1 {
        return 22
    } else {
        return 33
    }
}

const N = code(2)

fn main() {
    var arr [i32 N]
    assert N == 33
    assert arr[0] == 0
}
