import "platform"

fn main() {
	var x, y = 1, 2
	x, y = y, x

	var a [int 6]
	a[x], x = 3, 4
	x, a[x] = 5, 6

	if x != 5 {
		platform.exit(1)
	}
	if y != 1 {
		platform.exit(2)
	}
	if a[2] != 3 {
		platform.exit(3)
	}
	if a[5] != 6 {
		platform.exit(4)
	}

	platform.exit(0)
}
