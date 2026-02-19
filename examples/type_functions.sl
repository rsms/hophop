// type-function selector sugar with built-ins (`x.len()`, `ma.new(...)`)
import "std/mem" as mem

fn main() {
    var msg = "hi"
    var ma = 0 as mut&mem.Allocator

    assert msg.len() == len(msg)

    var p ?*i32 = ma.new(i32)
    var q ?*[i32 4] = ma.new(i32, 4)

    assert p == null
    assert q == null
}
