import "compiler"

fn only_runtime() {
	compiler.error("runtime call")
}
