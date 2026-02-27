struct Config {
	retries    i32
	timeout_ms i32
}

fn main() {
	var c Config = { timeout_ms = 500 }
	assert (c.retries == 0)
	assert (c.timeout_ms == 500)
}
