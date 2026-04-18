// Verifies the Wasm backend accepts array addr i16.
fn main() {
	var a [i16 2]
	var p *i16 = &a[0]

	*p = 7 as i16
	assert a[0] == 7 as i16
}
