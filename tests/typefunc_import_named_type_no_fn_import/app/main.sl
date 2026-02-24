import "lib/mem" { ArenaAllocator, ArenaAllocator as Arena, free_all }

fn main() {
    var arena ArenaAllocator
    arena.init(1024)
    assert arena.block_size == 1024

    free_all(&arena)
    assert arena.block_size == 0

    var arena2 Arena
    arena2.init(7)
    assert arena2.block_size == 7
}
