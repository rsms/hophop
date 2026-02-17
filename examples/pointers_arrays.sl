struct Node {
    value i32
    next  *Node
}

fn main() i32 {
    var values [i32 4]
    values[0] = 7

    var p *i32 = &values[0]
    var n Node
    n.value = *p
    n.next = 0 as *Node

    if n.next == 0 as *Node {
        return n.value
    }
    return 0
}
