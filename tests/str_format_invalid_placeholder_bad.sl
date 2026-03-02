import "core/str" { format }

fn main() {
	var out [u8 32]
	_ = format(buf: out, "x={x}", 1 as i64)
}
