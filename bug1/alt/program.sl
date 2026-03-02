import "alt/package" { shaped, simple }

fn main() {
	var x i64 = 1
	var y i64 = 2

	_ = x.simple(y)

	const FMT &str = "x={i}"
	var out [u8 16]
	_ = out.shaped(FMT, x)
}
