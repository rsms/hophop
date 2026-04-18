// Verifies type constructor const name not type is rejected.
const NotAType = 1

var x NotAType = 2

fn main() {
	_ = x
}
