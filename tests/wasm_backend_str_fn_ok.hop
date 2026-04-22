// Verifies the Wasm backend accepts string function.
import "platform"

fn id(msg &str) &str {
	return msg
}

fn main() {
	var msg = id("hello from helper")
	platform.console_log(msg, flags: 0)
}
