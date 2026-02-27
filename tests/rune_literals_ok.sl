fn main() {
	var a rune = 'a'
	var n rune = '\n'
	var c rune = '©'
	var b rune = '本'

	assert a == 0x0061 as rune
	assert n == 0x000A as rune
	assert c == 0x00A9 as rune
	assert b == 0x672C as rune

	var x i64 = 'अ'
	var y u32 = 'अ'
	var z i32 = 'अ'
	var u u16 = 'अ'
	var i i8  = 'A'
	var q u8  = 'å'

	assert x == 0x0905 as i64
	assert y == 0x0905 as u32
	assert z == 0x0905 as i32
	assert u == 0x0905 as u16
	assert i == 0x41 as i8
	assert q == 0xE5 as u8
}
