// Verifies string format argument type s is rejected.
import "str" { format }

fn main() {
	var out [u8 32]
	format(buf: out, "x={s}", 1 as i64)
}
