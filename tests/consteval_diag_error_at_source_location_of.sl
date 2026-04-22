// Verifies const-eval diagnostic error at source_location_of.
import "builtin"
import "compiler"

const X int = fail_at_mark()

fn fail_at_mark() int {
	compiler.error_at(builtin.source_location_of(MARK), "anchored")
	return 0
}

const MARK int = 1
