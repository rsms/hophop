// pointers, fixed-size arrays, indexing, and dereference operations
struct Node {
	value i32
	next  *Node
}

fn main() {
	var values [i32 4]
	values[0] = 7

	var p = &values[0]
	var n Node
	n.value = *p
	n.next = null as *Node

	if n.next == null as *Node {
		assert n.value == 7
	} else {
		assert 0 == 1
	}
}
