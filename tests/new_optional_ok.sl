import "std/testing"

fn main() {
    var ma *Allocator = 0 as *Allocator
    var n uint = 4
    var p ?*i32 = new(ma, i32)
    var q ?*[i32 4] = new(ma, i32, 4)
    var r ?*[i32] = new(ma, i32, n)
    assert p == null
    assert q == null
    assert r == null
}
