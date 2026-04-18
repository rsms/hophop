// Verifies const-eval diagnostic non const context is rejected.
import "compiler"

fn only_runtime() {
	compiler.error("runtime call")
}
