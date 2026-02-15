fn main() i32 {
    var sum i32 = 0
    var i i32 = 0

    for i < 10 {
        if i == 5 {
            i += 1
            continue
        }

        sum += i
        if sum > 20 {
            break
        }
        i += 1
    }

    switch sum {
    case 0 {
        return 0
    }
    default {
        return sum
    }
    }
}
