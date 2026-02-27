fn sum_to(n i32) i32 {
    var i i32 = 0
    var sum i32 = 0
    for i = 0; i < n; i += 1 {
        sum += i
    }
    return sum
}

const N = sum_to(5)

fn main() {
    var arr [i32 N]
    assert N == 10
    assert arr[0] == 0
}
