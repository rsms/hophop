// Verifies const-eval string index negative is rejected.
fn require_non_negative(const s &str) {
	const {
		assert s[-1 as int] == 'a'
	}
}

fn main() {
	require_non_negative("abc")
}
