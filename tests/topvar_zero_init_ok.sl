var g int

var bytes [u8 3]

fn main() {
	assert g == 0
	assert len(bytes) == 3
	assert bytes[0] == 0 as u8
	assert bytes[1] == 0 as u8
	assert bytes[2] == 0 as u8
}
