import "lib/dupe" as dupe

var f = dupe.helper

fn main() {
	assert f(10) == 15
}
