import "std/testing"
import "std/mem"

fn main() {
    var ma mut&mem.Allocator = 0 as mut&mem.Allocator
    var _p *i32 = new(ma, i32)
}
