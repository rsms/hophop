fn choose(flag bool, a, b i32) i32 {
	if flag {
		var v = a + 1
		return v
	} else {
		return b + 2
	}
}

const N = choose(true, a: 4, b: 9) + choose(false, a: 7, b: 3)

fn main() {
	var arr [i32 N]
	assert N == 10
	assert arr[0] == 0
}
