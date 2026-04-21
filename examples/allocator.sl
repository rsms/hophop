// Demonstrates `new` keyword forms:
// - explicit allocator: `new T context ma` or `new [T N] context ma`
// - contextual allocator: `new T` or `new [T N]` with `mem` in context
struct Pair {
	x i32
	y i32
}

fn explicit_forms(ma *Allocator, n uint) {
	var p     *Pair    = new Pair context ma
	var fixed *[i32 4] = new [i32 4] context ma
	var dyn   *[i32]   = new [i32 n] context ma

	p.x = 42
	p.y = 0
	fixed[0] = p.x
	dyn[0] = fixed[0]
	assert fixed[0] == 42
	assert dyn[0] == 42
	assert len(dyn) == n
}

fn contextual_forms(n uint) context struct {
	mem *Allocator
} {
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
	var ma     = context.mem
	var n uint = 6

	explicit_forms(ma, n)
	contextual_forms(n) context { mem: ma }
}
