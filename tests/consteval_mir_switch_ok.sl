fn classify(x int) int {
	switch x {
		case 2  { return 7 }
		default { return 3 }
	}
}

const VALUE int = classify(2)

fn main() {
	assert VALUE == 7
}
