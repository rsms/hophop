import "slang/feature/optional"

// if x == null { } (empty body — does NOT terminate) gives no continuation narrowing.
pub fn bad(x ?*i32) i32 {
    if x == null {
    }
    return *x
}
