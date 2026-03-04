import "str" { format }

fn main() {
	var out [u8 32]
	format(buf: out, "x={f}", 1 as i64)
}
