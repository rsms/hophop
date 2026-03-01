import "compiler"
import "reflect"

const X int = fail_at_mark()

fn fail_at_mark() int {
	compiler.error_at(reflect.span_of(MARK), "anchored")
	return 0
}

const MARK int = 1
