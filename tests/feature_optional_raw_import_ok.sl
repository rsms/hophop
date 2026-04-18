// Verifies feature optional raw import is accepted.
import `feature/optional`

fn main() {
	var x ?*i32 = null
	assert x == null
}
