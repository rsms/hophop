import "slang/feature/optional"

pub fn find(p ?*i32) bool {
    return p != null
}

pub fn unwrap_or_zero(p ?*i32) i32 {
    if p == null {
        return 0
    }
    return *p!
}

pub fn make_optional(x *i32) ?*i32 {
    return x
}

pub fn take_null() ?*i32 {
    return null
}
