// Demonstrates `new` keyword forms:
// - explicit allocator: `new T in ma` or `new [T N] in ma`
// - ambient allocator: `new T` or `new [T N]` using `context.allocator`
struct Pair {
	x i32
	y i32
}

fn explicit_forms(ma MemAllocator, n int) {
	var p     *Pair    = new Pair in ma
	var fixed *[i32 4] = new [i32 4] in ma
	var dyn   *[i32]   = new [i32 n] in ma

	p.x = 42
	p.y = 0
	fixed[0] = p.x
	dyn[0] = fixed[0]
	assert fixed[0] == 42
	assert dyn[0] == 42
	assert len(dyn) == n
}

fn ambient_forms(n int) {
	var p     *Pair    = new Pair
	var fixed *[i32 4] = new [i32 4]
	var dyn   *[i32]   = new [i32 n]

	p.x = 42
	p.y = 0
	fixed[0] = p.x
	dyn[0] = fixed[0]
	assert fixed[0] == 42
	assert dyn[0] == 42
	assert len(dyn) == n
}

fn main() {
	var ma    = context.allocator
	var n int = 6

	explicit_forms(ma, n)
	ambient_forms(n)
}
