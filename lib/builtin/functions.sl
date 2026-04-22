// Core declarations for language built-ins.
//
// Intrinsic behavior/signature details are defined by the compiler.
// See docs/library.md for the full call forms, especially for `new` and `sizeof`.
fn len(x &str) int {
	return x.len()
}

fn cstr(s &str) &u8 {
	return s.cstr()
}

fn copy(dst *[anytype], src &[anytype]) int {
	return 0
}

fn free() {}

fn panic(message &str) {}

pub fn print(message &str) context PrintContext {
	context.log.handler(&context.log, message, LogLevel.Info, 0 as LogFlags)
}

fn sizeof() int {
	return 0
}
