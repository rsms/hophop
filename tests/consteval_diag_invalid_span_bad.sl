// Verifies const-eval diagnostic invalid span is rejected.
import "compiler"
import "reflect"

const X int = fail_with_invalid_span()

fn fail_with_invalid_span() int {
	compiler.error_at(reflect.Span{ start: { line: 0, column: 0 }, end: { line: 0, column: 0 } }, "bad span")
	return 0
}
