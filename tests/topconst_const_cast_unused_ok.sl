fn trunc(x f64) i32 {
	return x as i32
}

const A i32 = 1

const B f64 = 1

const C i32 = 1.9

const D bool = 0

const E i32 = trunc(3.9)

var CAST_ARR [u8 3.9 as i32]

fn main() {
	assert A == 1
	assert B > 0.5
	assert C == 1
	assert D == false
	assert E == 3
	assert len(CAST_ARR) == 3
}
