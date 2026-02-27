fn choose(flag bool, a i32, b i32) i32 {
    if flag {
        const v = a + 1
        return v
    } else {
        return b + 2
    }
}

const N = choose(true, 4, 9) + choose(false, 7, 3)

fn main() {
    var arr [i32 N]
    assert N == 10
    assert arr[0] == 0
}
