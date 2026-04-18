// Verifies struct field defaults is accepted.
struct LoadFileContext {
	mem     i32
	tmpmem  i32 = mem
	retries i32 = tmpmem + 1
	fs      i32
}

fn main() {
	var a = LoadFileContext{ mem: 4, fs: 9 }
	assert (a.mem == 4)
	assert (a.tmpmem == 4)
	assert (a.retries == 5)
	assert (a.fs == 9)

	var b = LoadFileContext{ mem: 7, tmpmem: 20, fs: 1 }
	assert (b.mem == 7)
	assert (b.tmpmem == 20)
	assert (b.retries == 21)
	assert (b.fs == 1)

	var c = LoadFileContext{ mem: 2, tmpmem: 3, retries: 8, fs: 0 }
	assert (c.mem == 2)
	assert (c.tmpmem == 3)
	assert (c.retries == 8)
	assert (c.fs == 0)
}
