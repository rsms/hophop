fn count(x i32) i32 {
	assert x > 10
	return x + 1
}

const N = count(2)

fn main() {
	var a [i32 N]
	assert a[0] == 0
}
