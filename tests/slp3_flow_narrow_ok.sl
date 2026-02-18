import "slang/feature/optional"

// 1. Guard: if x == null { return } — x narrows to *i32 in continuation.
pub fn guard_null_return(x ?*i32) i32 {
    if x == null {
        return 0
    }
    return *x  // *x valid: x is *i32 here (not ?*i32)
}

// 2. Guard: if x != null { return } — x narrows to null in continuation.
pub fn guard_nonnull_return(x ?*i32) bool {
    if x != null {
        return true
    }
    return x == null  // valid: x is null-typed here
}

// 3. Inside if x != null { ... } — x narrows to *i32 inside the body.
pub fn nonnull_body(x ?*i32) i32 {
    if x != null {
        return *x  // *x valid: x is *i32 inside this branch
    }
    return 0
}

// 4. if x == null { ... } else { ... } — x narrows to *i32 in else.
pub fn ifelse_null_else(x ?*i32) i32 {
    if x == null {
        return 0
    } else {
        return *x  // *x valid: x is *i32 in else branch
    }
}

// 5. if x != null { ... } else { ... } — x narrows to *i32 in then, null in else.
pub fn ifelse_nonnull(x ?*i32) i32 {
    if x != null {
        return *x  // *x valid: x is *i32 in then branch
    } else {
        return 0   // x is null-typed in else branch
    }
}

// 6. Inside if x == null { ... } — x is null-typed (can compare, assign to ?T).
pub fn null_body(x ?*i32) ?*i32 {
    if x == null {
        return null  // x is null here; returning null for ?*i32 is valid
    }
    return x
}

// 7. Multiple guards in sequence — each one further narrows.
pub fn multi_guard(x ?*i32, y ?*i32) i32 {
    if x == null {
        return 0
    }
    if y == null {
        return *x  // x is *i32, y is null: use x safely
    }
    return *x + *y  // both narrowed to *i32
}

// 8. Guard with null == x (symmetric form).
pub fn guard_null_lhs(x ?*i32) i32 {
    if null == x {
        return 0
    }
    return *x  // x narrows to *i32
}

// 9. Guard with null != x (symmetric form).
pub fn guard_nonnull_lhs(x ?*i32) i32 {
    if null != x {
        return *x  // x narrows to *i32 inside body
    }
    return 0
}
