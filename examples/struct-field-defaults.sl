// struct field defaults, introduced in SLP-15
// - omitted fields can use per-field defaults
// - default expressions can reference earlier fields
// - explicit initializer overrides default
// - default expressions run in field declaration order
var trace i32 = 0

fn mark(v i32) i32 {
	trace = trace * 10 + v
	return v
}

struct Config {
	retries    i32 = 3
	timeout_ms i32 = retries * 1000
	mode       i32
}

struct Ordered {
	a i32
	b i32 = mark(a + 2)
	c i32 = mark(b + 3)
}

fn main() {
	// retries + timeout_ms come from defaults
	var a = Config{ mode = 1 }
	assert a.retries == 3
	assert a.timeout_ms == 3000
	assert a.mode == 1

	// explicit initializer overrides default for retries
	var b = Config{ retries = 5, mode = 2 }
	assert b.retries == 5
	assert b.timeout_ms == 5000
	assert b.mode == 2

	// ordering example:
	// explicit initializers evaluate first in source order (c then a),
	// then defaults evaluate in field declaration order (b then c-default if needed).
	var o = Ordered{ c = mark(9), a = mark(1) }
	assert trace == 913
	assert o.a == 1
	assert o.b == 3
	assert o.c == 9
}
