import "compiler"
import "reflect"

fn demo(args ...anytype) {
	const {
		compiler.warn_at(reflect.span_of(args[1]), "pack warn")
	}
}

fn main() {
	demo(1, "two", true)
}
