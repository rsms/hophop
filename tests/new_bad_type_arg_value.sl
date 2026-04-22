// Verifies new bad type argument value.
fn main() i32 {
	var ma MemAllocator
	var t  i32
	var p  *i32 = new t
	return p as i32
}
