import "slang/feature/optional"

pub fn find(p ?*i32) bool {
    return p != null
}

// Unwrap unconditionally — panics if p is null.
pub fn deref_unwrap(p ?*i32) i32 {
    return *p!
}

pub fn make_optional(x *i32) ?*i32 {
    return x
}

pub fn take_null() ?*i32 {
    return null
}
