// Verifies nested type unknown member is rejected.
struct Thing {
	struct Item {
		x i64
	}
}

fn main() {
	var x Thing.Missing
	_ = x
}
