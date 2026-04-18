// Verifies const block compiler error is rejected.
import "compiler"

fn demo() {
	const {
		compiler.error("boom")
	}
}

fn main() {
	demo()
}
