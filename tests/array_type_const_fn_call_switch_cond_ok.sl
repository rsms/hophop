fn norm(x i32) i32 {
    switch {
    case x < 0 {
        return 0
    }
    case x == 0 {
        return 1
    }
    default {
        return 2
    }
    }
}

fn main() {
    var a [i32 norm(7)]
    assert a[0] == 0
}
