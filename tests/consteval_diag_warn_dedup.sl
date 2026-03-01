import "compiler"

const A int = warn_once()

const B int = warn_once()

fn warn_once() int {
	compiler.warn("heads up")
	return 1
}
