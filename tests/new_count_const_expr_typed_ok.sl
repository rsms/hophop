// Verifies new count const expression typed is accepted.
const N = 3

fn main() {
	var ma        = context.mem
	var p *[u8 4] = new [u8 N + 1] with ma
	p[0] = 7
	assert p[0] == 7
}
