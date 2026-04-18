// Verifies slice bad unsliceable base.
fn main() {
	var p *i32
	var s &[i32] = p[1:]
	_ = s
}
