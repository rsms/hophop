import "platform"

fn record_order(x i32, out *i32) i32 {
	var y i32 = x
	defer *out = y
	defer y += 3
	return y
}

fn classify(x i32, out *i32) i32 {
	var y i32 = 0
	defer *out = y
	switch x {
		case 2 {
			y = 9
			return x + 5
		}
		default { return 0 }
	}
}

fn main() {
	var a i32 = 0
	var b i32 = 0
	if !(record_order(2, out: &a) == 5 && a == 5) {
		platform.exit(1)
	}
	if !(classify(2, out: &b) == 7 && b == 9) {
		platform.exit(2)
	}
}
