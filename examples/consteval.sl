// compile-time evaluation (const-eval / comptime) capabilities today:
// - const arithmetic and const-to-const references
// - const function calls
// - local const/var declarations inside const-evaluated functions
// - simple control flow in const-evaluated functions (if/else, nested blocks)
// - for-loops in const-evaluated functions (for cond { ... } and for init; cond; post { ... })
// - break/continue inside const-evaluated for-loops
// - switch statements in const-evaluated functions (expression and condition forms)
// - assert statements in const-evaluated functions
// - defer statements with const-evaluable deferred statements
// - float literals/arithmetic and optional-null literals in const initializers
// - sizeof(Type) and explicit casts (`as`) in const initializers
// - const expressions in array lengths and `new` sizes

const BASE = 3
const WIDTH = BASE * 4 + 1

fn stride(x i32) i32 {
    const doubled = x * 2
    return doubled + 1
}

fn checked_stride(x i32) i32 {
    assert x > 0
    return stride(x)
}

fn elem_size(x i32) i32 {
    return sizeof(x) as i32
}

fn local_elem_size() i32 {
    var x i32 = 1
    return sizeof(x) as i32
}

fn local_infer_elem_size() i32 {
    var x = 1
    return sizeof(x) as i32
}

const COUNT = stride(WIDTH)
const CHECKED = checked_stride(5)

fn sum_to(n i32) i32 {
    var i i32 = 0
    var sum i32 = 0
    for i < n {
        sum += i
        i += 1
    }
    return sum
}

const TRI = sum_to(6)

fn kind(x i32) i32 {
    switch x {
    case 0 {
        return 1
    }
    case 1, 2 {
        return 2
    }
    default {
        return 3
    }
    }
}

fn defer_value(x i32) i32 {
    var y i32 = x
    defer y += 1
    return y
}

fn odd_count_upto(limit i32) i32 {
    var i i32 = 0
    var count i32 = 0
    for i = 0; i < limit; i += 1 {
        if i == 10 {
            break
        }
        if i % 2 == 0 {
            continue
        }
        count += 1
    }
    return count
}

const ODD_COUNT = odd_count_upto(20)
const KIND = kind(2)
const DEFER_VALUE = defer_value(4)
const PI f64 = 3.14159
const NONE ?*i32 = null
const SIZE_I32 = sizeof(i32)
const SIZE_PI_INT = sizeof(PI_INT)
const PI_INT i32 = PI as i32
const ELEM_SIZE = elem_size(123)
const LOCAL_ELEM_SIZE = local_elem_size()
const LOCAL_INFER_ELEM_SIZE = local_infer_elem_size()
const ZERO_FROM_NULL = null as int
const TRUE_FROM_STRING = "x" as bool

fn main() {
    var ma = context.mem

    var stack [i32 WIDTH]
    var heap = new [u8 COUNT + 2] with ma
    var tri [i32 TRI]

    assert WIDTH == 13
    assert COUNT == 27
    assert CHECKED == 11
    assert TRI == 15
    assert ODD_COUNT == 5
    assert KIND == 2
    assert DEFER_VALUE == 5
    assert PI > 3.0
    assert PI_INT == 3
    assert SIZE_I32 == 4
    assert SIZE_PI_INT == 4
    assert ELEM_SIZE == 4
    assert LOCAL_ELEM_SIZE == 4
    assert LOCAL_INFER_ELEM_SIZE == sizeof(1 as int) as i32
    assert ZERO_FROM_NULL == 0
    assert TRUE_FROM_STRING == true
    assert NONE == null
    assert stack[0] == 0
    assert tri[0] == 0
    assert heap == null
}
