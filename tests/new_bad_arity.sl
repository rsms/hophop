// Verifies new bad arity.

fn main() i32 {
    var ma MemAllocator
    var p *i32 = new(&ma, i32, 1, 2)
    return p as i32
}
