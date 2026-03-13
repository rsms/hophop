import "lib/dupe" as dupe

var Answer = 7

var Copy = Answer

fn main() {
	dupe.helper()
	assert Copy == 7
}
