import "std/testing"
import "std/mem"

fn main() {
    var ma *Allocator = 0 as *Allocator
    var _p *i32 = ma.new(i32)
}
