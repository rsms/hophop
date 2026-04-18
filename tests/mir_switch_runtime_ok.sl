// Verifies MIR runtime behavior for switch.
import "platform"

fn main() {
	var y = 0

	switch 2 {
		case 0 { y = 10 }
		case 1, 2 {
			y = 20
			break
		}
		default { platform.exit(1) }
	}
	if y != 20 {
		platform.exit(2)
	}

	var sum = 0
	var i   = 0
	for i < 4 {
		switch {
			case i == 0 {
				i += 1
				continue
			}
			case i == 2 { break }
			default     { sum += i }
		}
		i += 1
	}
	if !(sum == 4 && i == 4) {
		platform.exit(3)
	}
}
