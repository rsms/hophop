// type-function selector sugar with user-defined and built-in functions
import "std/mem"

struct Counter {
    value int
}

fn doubled(c Counter) int {
    return c.value * 2
}

fn main() {
    var c Counter
    c.value = 21

    var msg = "hi"
    var ma = 0 as mut&mem.Allocator

    assert c.doubled() == doubled(c)
    assert msg.len() == len(msg)

    var p ?*i32 = ma.new(i32)
    var q ?*[i32 4] = ma.new(i32, 4)

    assert p == null
    assert q == null
}
