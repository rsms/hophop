// Verifies duplicate non-blank names in short assignment are rejected.
fn f() {
	x, x := 1, 2
}
