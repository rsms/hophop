// Verifies new bad allocator read-only.
fn main() i32 {
	var ma Allocator
	var ro &Allocator = &ma
	var p  *i32       = new i32 context ro
	return p as i32
}
