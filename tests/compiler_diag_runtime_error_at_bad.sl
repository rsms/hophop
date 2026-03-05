import "compiler"
import "reflect"

fn runtime_error_at() {
	compiler.error_at(reflect.span_of(MARK), "runtime anchored")
}

const MARK int = 1
