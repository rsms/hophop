import "lib/mem" { ArenaAllocator }

fn main() {
    var arena ArenaAllocator
    init(&arena, 1024)
}
