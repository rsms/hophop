import "compiler"

fn runtime_const_if_false() {
	if sizeof(int) < sizeof(i32) {
		compiler.error("should not fire")
	}
}

fn main() {
	runtime_const_if_false()
}
