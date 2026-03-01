import "compiler"

var MSG &str = "not const"

const X int = fail_with_non_const_message()

fn fail_with_non_const_message() int {
	compiler.error(MSG)
	return 0
}
