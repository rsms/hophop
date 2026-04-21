// Verifies context is accepted.
struct Ctx {
	mem      *Allocator
	temp_mem *Allocator
	log      Logger
}

fn alloc() *i32 context Ctx {
	return new i32
}

fn log(msg &str) context Ctx {
	print(msg)
}

fn caller() context Ctx {
	var p *i32 = alloc()

	var p2 *i32 = alloc() context { mem, temp_mem, log }

	var p3 ?*i32 = new i32 context context.mem

	log("ok") context context
}

fn caller_direct(ctx *Ctx) context Ctx {
	log("direct") context ctx
}
