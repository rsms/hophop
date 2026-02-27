// single-file package mode compatible with checkpkg/genpkg commands
fn helper(x i32) i32 {
	return x * 3
}

fn main() {
	assert helper(14) == 42
}
