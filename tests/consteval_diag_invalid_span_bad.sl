// Verifies const-eval diagnostic invalid span is rejected.
import "builtin"
import "compiler"

const X int = fail_with_invalid_span()

fn fail_with_invalid_span() int {
	compiler.error_at(builtin.SourceLocation{ start_line: 0, start_column: 0, end_line: 0, end_column: 0 }, "bad span")
	return 0
}
