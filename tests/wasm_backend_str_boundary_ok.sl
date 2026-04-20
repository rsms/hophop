import "platform"

@export("sl_test_str")
pub fn sl_test_str(message &str) i32 {
	platform.console_log(message, flags: 0)
	return message.len as i32
}

fn main() i32 {
	return 0
}
