// Verifies const block compiler error at pack span is rejected.
import "compiler"
import "reflect"

fn demo(args ...anytype) {
	const {
		compiler.error_at(reflect.span_of(args[1]), "pack boom")
	}
}

fn main() {
	demo(1, "two", true)
}
