import "std/testing"

fn main() {
    var ma mut&MemAllocator = 0 as mut&MemAllocator
    var _p *i32 = ma.new(i32)
}
