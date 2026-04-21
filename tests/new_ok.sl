// Verifies new is accepted.
fn main() {
	var ma          = context.mem
	var _p *i32     = new i32 context ma
	var _q *[i32 4] = new [i32 4] context ma
}
