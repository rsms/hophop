// Verifies runtime behavior for compiler diagnostic const if true.
import "compiler"

fn runtime_const_if_true() {
	if sizeof(int) >= sizeof(i32) {
		compiler.error("int is too small")
	}
}
