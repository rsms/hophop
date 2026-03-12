import "platform"

fn main() {
	var x, y = 1, 2
	x, y = y, x
	if !(x == 2 && y == 1) {
		platform.exit(1)
	}

	var a [int 6]
	a[x], x = 3, 4
	if !(a[2] == 3 && x == 4) {
		platform.exit(2)
	}

	x, a[x] = 5, 6
	if !(x == 5 && a[5] == 6) {
		platform.exit(3)
	}
}
