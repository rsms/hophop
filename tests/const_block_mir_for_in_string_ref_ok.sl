// Verifies const block MIR for in string reference is accepted.
fn sum_text() u32 {
	var text &str = "ABC"
	var sum  u32
	for &ch in text {
		var byte u8 = *ch
		sum += byte as u32
	}
	return sum
}

fn main() {
	const {
		assert sum_text() == 'A' as u32 + 'B' as u32 + 'C' as u32
	}
}
