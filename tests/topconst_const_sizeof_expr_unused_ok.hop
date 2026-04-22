// Verifies topconst const sizeof expression unused is accepted.
const X = 7

const SX = sizeof(X)

const SY = sizeof(1 as i32)

var A [u8 sizeof(X)]

fn main() {
	assert SX == 8
	assert SY == 4
	assert len(A) == 8
}
