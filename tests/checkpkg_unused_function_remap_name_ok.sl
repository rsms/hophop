struct KVEntry {
	key &str
}

fn f(e *KVEntry) &str {
	return e.key
}

fn main() {}
