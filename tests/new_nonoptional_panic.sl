import "std/testing"
import "std/mem"

fn main() {
    var ma *mem.Allocator = 0 as *mem.Allocator
    var _p *i32 = new(ma, i32)
}
