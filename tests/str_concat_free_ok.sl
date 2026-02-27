struct Ctx {
	mem *Allocator
}

fn caller() context Ctx {
	var r &str = "abc"
	var s *str = concat(r, r)
	assert len(s) == 6
	free(s)

	var arr *[u8 4] = new [u8 4]
	free(arr)
}
