// Verifies ambient platform context main is accepted.
import "platform"

fn alloc() *u8 {
	return new u8
}

fn main() {
	var p *u8 = alloc()
	del p
}
