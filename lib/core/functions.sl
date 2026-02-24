// Core declarations for language built-ins.
//
// Intrinsic behavior/signature details are defined by the compiler.
// See docs/library.md for the full call forms, especially for `new` and `sizeof`.

fn len(x &str) u32 {
    return x.len()
}

fn cstr(s &str) &u8 {
    return s.cstr()
}

fn concat(a &str, b &str) *str {
    return 0 as *str
}

fn free() {}

fn panic(message &str) {}

pub fn print(message &str) context PrintContext {
    context.log.handler(&context.log, message, LogLevel.Info, 0 as LogFlags)
}

fn sizeof() uint {
    return 0
}
