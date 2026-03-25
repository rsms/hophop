fn strlen3() int {
	return len("abc") as int
}

const N = strlen3()

var A [u8 N]

fn main() {
	assert len(A) == 3
}
