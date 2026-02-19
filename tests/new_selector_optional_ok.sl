import "std/testing"
import "std/mem"

fn main() {
    var ma mut&mem.Allocator = 0 as mut&mem.Allocator
    var n uint = 4
    var p ?*i32 = ma.new(i32)
    var q ?*[i32 4] = ma.new(i32, 4)
    var r ?*[i32] = ma.new(i32, n)
    assert p == null
    assert q == null
    assert r == null
}
