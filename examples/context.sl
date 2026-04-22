// Ambient context is available in every function body.
fn alloc_value() *i32 {
	var p *i32 = new i32
	*p = 42
	return p
}

fn with_prefix(msg &str) {
	context.logger.prefix = "[sl] "
	print(msg)
}

fn main() {
	var p *i32 = alloc_value()
	assert *p == 42

	{
		with_prefix("inner")
	}

	print("outer")
	del p
}
