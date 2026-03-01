// compile-time evaluation (const-eval / comptime) in a small example.
//
// For exhaustive feature coverage, see tests/consteval_catalog.sl.
const BASE = 3

fn width(n int) int {
	return n*4 + 1
}

fn triangle(n int) int {
	var i   int = 0
	var sum int = 0
	for i < n {
		sum += i
		i += 1
	}
	return sum
}

fn classify(x int) int {
	if x < 0 {
		return -1
	}
	switch x {
		case 0    { return 10 }
		case 1, 2 { return 20 }
		default   { return 30 }
	}
}

const WIDTH = width(BASE)

const TRI = triangle(6)

const KIND = classify(2)

const I32_SIZE = sizeof(i32) as int

const PI_INT int = 3.14159

fn main() {
	// Use literal lengths for now: named-const lengths are still a codegen gap.
	var a [i32 13]
	var b [u8 15]

	assert WIDTH == 13
	assert TRI == 15
	assert KIND == 20
	assert I32_SIZE == 4
	assert PI_INT == 3
	assert a[0] == 0
	assert b[0] == 0
}
