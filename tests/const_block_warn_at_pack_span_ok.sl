// Verifies const block warning at pack span is accepted.
import "compiler"
import "reflect"

fn demo(args ...anytype) {
	const {
		compiler.warn_at(reflect.span_of(args[1]), "pack warn")
	}
	if args.len() > 0 {
		_ = args[0]
	}
}

fn main() {
	demo(1, "two", true)
}
