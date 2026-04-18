// Verifies string for in u8 is accepted.
fn main() {
	var text &str = "xyz"

	var sum u32
	for ch in text {
		sum += ch as u32
	}
	assert sum == 'x' as u32 + 'y' as u32 + 'z' as u32

	var first u8
	for &ch in text {
		first = *ch
		break
	}
	assert first == 'x'
}
