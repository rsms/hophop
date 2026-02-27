fn id(x i32) i32 {
	return x
}

fn extent(x i32) i32 {
	id(x + 1)
	return x + 2
}

const N = extent(3)

fn main() {
	var arr [i32 N]
	assert N == 5
	assert arr[0] == 0
}
