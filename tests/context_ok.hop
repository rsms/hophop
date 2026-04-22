// Verifies ambient context is accepted.
fn alloc() *i32 {
	return new i32
}

fn log_msg(msg &str) {
	print(msg)
}

fn caller() {
	var p *i32 = alloc()
	del p

	{
		context.logger.prefix = "inner"
		log_msg("ok")
	}

	log_msg("done")
}

fn main() {
	caller()
}
