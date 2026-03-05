import "compiler"

fn runtime_warn() {
	compiler.warn("heads up runtime")
}

fn main() {
	runtime_warn()
}
