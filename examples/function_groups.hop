// same-name overloads and selector-call dispatch
struct Cat {
	score int
}

struct Dog {
	score int
}

fn pick(v Cat) int {
	return v.score
}

fn pick(v Dog) int {
	return v.score
}

fn main() {
	var cat Cat
	var dog Dog

	cat.score = 9
	dog.score = 4

	assert pick(cat) == 9
	assert dog.pick() == 4
}
