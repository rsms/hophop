import "std/testing"

fn main() i32 {
    var ma mut&MemAllocator = 0 as mut&MemAllocator
    var p *i32 = new(ma, i32)
    return (p == 0 as *i32) as i32
}
