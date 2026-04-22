// Verifies runtime behavior for compiler diagnostic error at.
import "builtin"
import "compiler"

fn runtime_error_at() {
	compiler.error_at(builtin.source_location_of(MARK), "runtime anchored")
}

const MARK int = 1
