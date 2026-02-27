fn next(x i32) i32 {
	var y i32 = x + 1
	return y
}

const N = next(4)

fn main() {
	var arr [i32 N]
	assert N == 5
	assert arr[0] == 0
}
