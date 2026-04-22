// Verifies short variable declarations at runtime.
import "platform"

fn pair() (int, int) {
	return 2, 3
}

fn main() {
	x := 1
	x, y := 4, 5
	a, b := pair()
	x, z := y, x
	sum := x + z + a + b
	for i := 0; i < 3; i += 1 {
		sum += i
	}
	_, w := sum, 8
	_ := w
	platform.exit(sum as i32)
}
