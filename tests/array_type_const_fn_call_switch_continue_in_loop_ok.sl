fn score(n i32) i32 {
    var i i32 = 0
    var sum i32 = 0
    for i = 0; i < n; i += 1 {
        switch i {
        case 0 {
            continue
        }
        default {
        }
        }
        sum += 1
    }
    return sum
}

fn main() {
    var a [i32 score(4)]
    assert a[0] == 0
}
