import "compiler"

const X int = fail_now()

fn fail_now() int {
	compiler.error("boom")
	return 0
}
