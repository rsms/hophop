// Verifies topconst const cast string bool is accepted.
const NONEMPTY bool = "x" as bool

const EMPTY bool = "" as bool

fn main() {
	assert NONEMPTY == true
	assert EMPTY == true
}
