// anonymous struct/union aggregate types
// - inline aggregate field types in named structs
// - anonymous aggregate params (by value and by ref)
// - anonymous context clauses and call overlays
// - inferred local anonymous aggregate literals

struct Example {
    pos { x, y i32 }
    size struct { w, h i32 }
    value union { i i32, f f64 }
}

fn point_sum(p { x, y i32 }) i32 {
    return p.x + p.y
}

fn point_sum_ref(p &{ x, y i32 }) i32 {
    return p.x + p.y
}

fn choose(v union { i i32, f f64 }) i32 {
    // Union values hold one active field at a time.
    return v.i
}

fn move_to(e Example, p struct { x, y i32 }) Example {
    // Function params are passed by value by default, so we return the updated copy.
    var out Example = e
    out.pos = p
    return out
}

fn set_value(e Example, v union { i i32, f f64 }) Example {
    var out Example = e
    out.value = v
    return out
}

fn read_pair(v { a i32, b str }) i32 {
    return v.a
}

fn announce(msg str) context { console u64 } {
    // `context { ... }` declares required ambient capabilities for this call.
    print(msg)
}

fn run() i32 context { console u64 } {
    var ex Example = {
        // Field values can use inferred anonymous literals too.
        pos = { x = 1, y = 2 },
        size = { w = 3, h = 4 },
        // Union literal initializes one field.
        value = { i = 5 },
    }

    ex = move_to(ex, { x = 11, y = 22 })
    ex = set_value(ex, { i = 9 })

    // Inferred local anonymous struct type from field names + value types.
    var inferred = { a = (7 as i32), b = "hello" }
    var from_inferred i32 = read_pair(inferred)

    announce(inferred.b)
    // `with { ... }` overlays call-local context values.
    announce("overlay") with { console }

    // `point_sum_ref` expects `&{...}`, so we pass a reference explicitly.
    return ex.size.w * ex.size.h + point_sum(ex.pos) + point_sum_ref(&ex.pos) + choose(ex.value)
        + from_inferred
}

fn main() {
    // `main` has no implicit context; provide required fields at the call site.
    var total i32 = run() with { console = 0 }
    assert total == 94
}
