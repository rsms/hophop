import "platform"

@export("sl_test")
pub fn sl_test() i32 {
	platform.console_log("hello from wasm export", flags: 0)
	return 7
}

fn main() i32 {
	return 0
}
