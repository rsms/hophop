// Verifies const block compiler error at pack span is rejected.
import "builtin"
import "compiler"

fn demo(args ...anytype) {
	const {
		compiler.error_at(builtin.source_location_of(args[1]), "pack boom")
	}
}

fn main() {
	demo(1, "two", true)
}
