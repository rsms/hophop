// Core declarations for language built-ins.
//
// Intrinsic behavior/signature details are defined by the compiler.
// See docs/library.md for the full call forms, especially for `new` and `sizeof`.

fn len(x str) u32 {
    return x.len()
}

fn cstr(s str) *u8 {
    return s.cstr()
}

fn new() *u8 {
    return 0 as *u8
}

fn panic(message str) {}

fn print(message str) {}

fn sizeof() uint {
    return 0
}
