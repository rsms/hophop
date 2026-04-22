// Verifies copy builtin is accepted.
fn main() {
	var src [u8 6]
	src[0] = 'a' as u8
	src[1] = 'b' as u8
	src[2] = 'c' as u8
	src[3] = 'd' as u8
	src[4] = 'e' as u8
	src[5] = 'f' as u8

	var dst [u8 4]
	var n1  int = copy(dst[:], src[:])
	assert n1 == 4
	assert dst[0] == 'a' as u8
	assert dst[1] == 'b' as u8
	assert dst[2] == 'c' as u8
	assert dst[3] == 'd' as u8

	var overlapRight [u8 6]
	overlapRight[0] = 'a' as u8
	overlapRight[1] = 'b' as u8
	overlapRight[2] = 'c' as u8
	overlapRight[3] = 'd' as u8
	overlapRight[4] = 'e' as u8
	overlapRight[5] = 'f' as u8
	var n2 int = copy(overlapRight[1:], overlapRight[:5])
	assert n2 == 5
	assert overlapRight[0] == 'a' as u8
	assert overlapRight[1] == 'a' as u8
	assert overlapRight[2] == 'b' as u8
	assert overlapRight[3] == 'c' as u8
	assert overlapRight[4] == 'd' as u8
	assert overlapRight[5] == 'e' as u8

	var overlapLeft [u8 6]
	overlapLeft[0] = 'a' as u8
	overlapLeft[1] = 'b' as u8
	overlapLeft[2] = 'c' as u8
	overlapLeft[3] = 'd' as u8
	overlapLeft[4] = 'e' as u8
	overlapLeft[5] = 'f' as u8
	var n3 int = copy(overlapLeft[:5], overlapLeft[1:])
	assert n3 == 5
	assert overlapLeft[0] == 'b' as u8
	assert overlapLeft[1] == 'c' as u8
	assert overlapLeft[2] == 'd' as u8
	assert overlapLeft[3] == 'e' as u8
	assert overlapLeft[4] == 'f' as u8
	assert overlapLeft[5] == 'f' as u8

	var dstStr *str = "zzzzz"
	var srcStr &str = "ABCDE"
	var n4     int  = copy(dstStr, srcStr)
	assert n4 == 5
	assert dstStr[0] == 'A' as u8
	assert dstStr[1] == 'B' as u8
	assert dstStr[2] == 'C' as u8
	assert dstStr[3] == 'D' as u8
	assert dstStr[4] == 'E' as u8

	var out [u8 3]
	var n5  int = copy(out[:], srcStr)
	assert n5 == 3
	assert out[0] == 'A' as u8
	assert out[1] == 'B' as u8
	assert out[2] == 'C' as u8
}
