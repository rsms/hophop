// Verifies checkpkg unused anypack parameter warning is accepted.
fn sink(args ...anytype) {}

fn main() {
	sink(1, "x", true)
}
