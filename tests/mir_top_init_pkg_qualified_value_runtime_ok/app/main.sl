import "lib/dupe" as dupe

var f = dupe.helper

var Sum = f(10)

fn main() {
	assert f(10) == 15
	assert Sum == 15
}
