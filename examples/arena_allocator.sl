import "mem"

fn init(self *mem.ArenaAllocator, source *Allocator, block_size uint) {
    mem.init(self, source, block_size)
}

fn free_all(self *mem.ArenaAllocator) {
    mem.free_all(self)
}

fn fill_values(ma *Allocator) i32 {
    var p *i32 = new i32 with ma
    *p = 10

    var n uint = 4
    var xs *[i32] = new [i32 n] with ma
    xs[0] = 1
    xs[1] = 2
    xs[2] = 3
    xs[3] = 4
    return *p + xs[0] + xs[1] + xs[2] + xs[3]
}

fn main() {
    var arena mem.ArenaAllocator
    arena.init(context.mem, 1024)

    var sum = fill_values(&arena)
    assert sum == 20

    arena.free_all()
}
