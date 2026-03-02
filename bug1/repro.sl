import "core/str" { format }

fn main() {
	var out [u8 32]
	_ = out.format("x={f}", 1 as i64)
}
