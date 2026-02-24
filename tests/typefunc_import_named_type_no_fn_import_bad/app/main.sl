import "lib/mem" { ArenaAllocator, free_all }

fn main() {
    var arena ArenaAllocator
    init(&arena, 1024)
    free_all(&arena)
}
