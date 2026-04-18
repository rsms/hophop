// Verifies const-eval branch specialize anytype is accepted.
fn score(x anytype) i64 {
	if typeof(x) == i64 {
		// The else branch is not type-correct for i64 instantiation.
		return (x + 1 as i64) - 1 as i64
	} else {
		// The then branch is not type-correct for &str instantiation.
		return len(x) as i64
	}
}

fn main() {
	assert score(7 as i64) == 7 as i64
	assert score("hello") == 5 as i64
}
