fn count_odds(limit i32) i32 {
    var i i32 = 0
    var count i32 = 0
    for i = 0; i < limit; i += 1 {
        if i % 2 == 0 {
            continue
        }
        count += 1
    }
    return count
}

fn main() {
    var a [i32 count_odds(6)]
    assert a[0] == 0
}
