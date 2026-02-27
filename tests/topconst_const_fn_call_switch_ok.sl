fn classify(x i32) i32 {
	var y i32 = 0
	defer y = x
	switch x {
		case 0    { return 1 }
		case 1, 2 { return 2 }
		default   { return 3 }
	}
}

const N = classify(2)

fn main() {
	var a [i32 N]
	assert N == 2
	assert a[0] == 0
}
