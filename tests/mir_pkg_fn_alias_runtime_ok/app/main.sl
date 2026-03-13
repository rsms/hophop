import "lib/dupe" as dupe

fn helper(x i32) i32 {
	return x + 1
}

var f = helper

fn main() {
	assert dupe.helper(2) == 99
	assert f(2) == 3
}
